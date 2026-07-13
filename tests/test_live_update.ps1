[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Executable,

    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
    [string]$ExpectedVersion
)

$ErrorActionPreference = 'Stop'
$smokeExecutable = (Resolve-Path -LiteralPath $Executable).Path
$installDir = Join-Path $env:LOCALAPPDATA 'Programs\VGPU-Mon'
$installedExe = Join-Path $installDir 'vgpu.exe'
$uninstaller = Join-Path $installDir 'unins000.exe'

function Get-UserPathState {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment')
    try {
        [pscustomobject]@{
            Value = [string]$key.GetValue(
                'Path',
                $null,
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames
            )
            Kind = $key.GetValueKind('Path').ToString()
        }
    }
    finally {
        $key.Close()
    }
}

function Invoke-HiddenProcess {
    param([string]$FilePath, [string[]]$Arguments)
    $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments `
        -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0) {
        throw "$FilePath exited with code $($process.ExitCode)."
    }
}

if (Test-Path -LiteralPath $installDir) {
    throw "Refusing to overwrite an existing installation at $installDir."
}

$before = Get-UserPathState
$beforeTemporaryFiles = @(
    Get-ChildItem -LiteralPath ([IO.Path]::GetTempPath()) -Filter 'VGPU-Mon-*' -File `
        -ErrorAction SilentlyContinue | ForEach-Object FullName
)

function Get-NewUpdaterTemporaryFiles {
    @(
        Get-ChildItem -LiteralPath ([IO.Path]::GetTempPath()) -Filter 'VGPU-Mon-*' -File `
            -ErrorAction SilentlyContinue | Where-Object {
                $_.FullName -notin $beforeTemporaryFiles
            }
    )
}

function Wait-UpdaterTemporaryCleanup {
    param([int]$Seconds)
    $cleanupDeadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        $files = @(Get-NewUpdaterTemporaryFiles)
        if ($files.Count -eq 0) { return @() }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $cleanupDeadline)
    return $files
}

$installed = $false
try {
    $handoffStarted = $false
    for ($attempt = 1; $attempt -le 12; $attempt++) {
        $process = Start-Process -FilePath $smokeExecutable -Wait -PassThru -NoNewWindow
        if ($process.ExitCode -eq 0) {
            $handoffStarted = $true
            break
        }
        if ($attempt -lt 12) {
            Start-Sleep -Seconds 5
        }
    }
    if (-not $handoffStarted) {
        throw 'The live updater could not resolve the newly published release.'
    }

    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    do {
        Start-Sleep -Milliseconds 500
        if (Test-Path -LiteralPath $installedExe) {
            $versionInfo = (Get-Item -LiteralPath $installedExe).VersionInfo
            if ($versionInfo.FileVersion -eq $ExpectedVersion -and
                (Test-Path -LiteralPath $uninstaller)) {
                $installed = $true
                break
            }
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $installed) {
        throw "The updater did not install VGPU-Mon $ExpectedVersion before the timeout."
    }

    $unexpectedRelaunch = @(Get-Process -Name 'vgpu' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path -ieq $installedExe })
    if ($unexpectedRelaunch.Count -ne 0) {
        throw 'The updater relaunched a foreground monitor after returning terminal ownership.'
    }

    $remainingTemporaryFiles = @(Wait-UpdaterTemporaryCleanup -Seconds 30)
    if ($remainingTemporaryFiles.Count -ne 0) {
        throw "The updater handoff did not finish: $($remainingTemporaryFiles.FullName -join ', ')"
    }

    $version = & $installedExe --no-update --version
    if ($LASTEXITCODE -ne 0 -or $version -ne "VGPU-Mon $ExpectedVersion") {
        throw "The updated executable failed its version smoke test: $version"
    }

    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $pathEntries = @($userPath -split ';' | Where-Object {
        $_.Trim().Trim('"').TrimEnd('\') -ieq $installDir
    })
    if ($pathEntries.Count -ne 1) {
        throw "The updater produced $($pathEntries.Count) VGPU-Mon PATH entries."
    }

    Invoke-HiddenProcess $uninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    $installed = $false
}
finally {
    if (Test-Path -LiteralPath $uninstaller) {
        Invoke-HiddenProcess $uninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    }
}

$after = Get-UserPathState
if ($after.Value -cne $before.Value -or $after.Kind -cne $before.Kind) {
    throw 'The live update lifecycle changed the original user PATH.'
}
if (Test-Path -LiteralPath $installDir) {
    throw 'The live update lifecycle left the application directory behind.'
}
if (Test-Path -LiteralPath 'HKCU:\Software\VGPU-Mon') {
    throw 'The live update lifecycle left installer registry state behind.'
}

$newTemporaryFiles = @(Wait-UpdaterTemporaryCleanup -Seconds 10)
if ($newTemporaryFiles.Count -ne 0) {
    throw "Updater temporary files were not cleaned: $($newTemporaryFiles.FullName -join ', ')"
}

Write-Host "Live automatic update, clean terminal return, PATH, cleanup, and uninstall tests passed for $ExpectedVersion."
