[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$InstallerPath
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$versionHeader = Get-Content -LiteralPath (Join-Path $root 'src\version.h') -Raw
$versionMatch = [regex]::Match($versionHeader, '#define\s+VGPU_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"')
if (-not $versionMatch.Success) {
    throw 'Could not read VGPU_VERSION from src\version.h.'
}
$expectedVersion = $versionMatch.Groups[1].Value
$installer = (Resolve-Path -LiteralPath $InstallerPath).Path
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
    throw "Refusing to overwrite an existing test installation at $installDir."
}

$before = Get-UserPathState
$installed = $false
try {
    Invoke-HiddenProcess $installer @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    $installed = $true
    if (-not (Test-Path -LiteralPath $installedExe)) {
        throw 'Installer did not create vgpu.exe.'
    }
    $versionInfo = (Get-Item -LiteralPath $installedExe).VersionInfo
    if ($versionInfo.FileVersion -ne $expectedVersion -or $versionInfo.ProductVersion -ne $expectedVersion) {
        throw "Installed executable has incorrect version metadata: $($versionInfo.FileVersion) / $($versionInfo.ProductVersion)"
    }

    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $entries = @($userPath -split ';' | Where-Object {
        $_.Trim().Trim('"').TrimEnd('\') -ieq $installDir
    })
    if ($entries.Count -ne 1) {
        throw "Expected one VGPU-Mon PATH entry; found $($entries.Count)."
    }

    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $oldProcessPath = $env:Path
    try {
        $env:Path = "$userPath;$machinePath"
        $command = Get-Command vgpu -ErrorAction Stop
        if ($command.Source -ine $installedExe) {
            throw "vgpu resolved to an unexpected executable: $($command.Source)"
        }
        $version = & vgpu --version
        if ($LASTEXITCODE -ne 0 -or $version -ne "VGPU-Mon $expectedVersion") {
            throw "vgpu smoke test failed: $version"
        }
    }
    finally {
        $env:Path = $oldProcessPath
    }

    # An in-place upgrade must retain ownership of the existing PATH entry and
    # must not append a duplicate.
    Invoke-HiddenProcess $installer @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    $upgradedPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $upgradedEntries = @($upgradedPath -split ';' | Where-Object {
        $_.Trim().Trim('"').TrimEnd('\') -ieq $installDir
    })
    if ($upgradedEntries.Count -ne 1) {
        throw "Upgrade produced $($upgradedEntries.Count) VGPU-Mon PATH entries."
    }

    if (-not (Test-Path -LiteralPath $uninstaller)) {
        throw 'Installer did not create its uninstaller.'
    }
    Invoke-HiddenProcess $uninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    $installed = $false
}
finally {
    if ($installed -and (Test-Path -LiteralPath $uninstaller)) {
        Invoke-HiddenProcess $uninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    }
}

$after = Get-UserPathState
if ($after.Value -cne $before.Value -or $after.Kind -cne $before.Kind) {
    throw "Uninstall changed the original user PATH (before $($before.Value.Length)/$($before.Kind), after $($after.Value.Length)/$($after.Kind))."
}
if (Test-Path -LiteralPath $installDir) {
    throw 'Uninstall left the application directory behind.'
}
if (Test-Path -LiteralPath 'HKCU:\Software\VGPU-Mon') {
    throw 'Uninstall left VGPU-Mon installer state behind.'
}

Write-Host 'Installer install, PATH command, and uninstall restoration tests passed.'
