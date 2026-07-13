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

$checksumPath = Join-Path ([IO.Path]::GetTempPath()) "vgpu-mon-checksums-$([Guid]::NewGuid().ToString('N')).txt"
$bootstrapInstalled = $false
try {
    $installerHash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
    "$('0' * 64)  $(Split-Path -Leaf $installer)" |
        Set-Content -LiteralPath $checksumPath -Encoding ascii
    $rejectedBadChecksum = $false
    try {
        & (Join-Path $root 'install.ps1') -InstallerPath $installer -ChecksumPath $checksumPath
    }
    catch {
        if ($_.Exception.Message -like 'SHA-256 verification failed*') {
            $rejectedBadChecksum = $true
        }
        else {
            throw
        }
    }
    if (-not $rejectedBadChecksum) {
        throw 'Bootstrap installer accepted an invalid SHA-256 checksum.'
    }
    if (Test-Path -LiteralPath $installDir) {
        throw 'Bootstrap installer ran setup after checksum verification failed.'
    }

    "$installerHash  $(Split-Path -Leaf $installer)" |
        Set-Content -LiteralPath $checksumPath -Encoding ascii

    & (Join-Path $root 'install.ps1') -InstallerPath $installer -ChecksumPath $checksumPath
    $bootstrapInstalled = $true
    if (-not (Test-Path -LiteralPath $installedExe)) {
        throw 'Bootstrap installer did not create vgpu.exe.'
    }
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $entries = @($userPath -split ';' | Where-Object {
        $_.Trim().Trim('"').TrimEnd('\') -ieq $installDir
    })
    if ($entries.Count -ne 1) {
        throw "Bootstrap installer produced $($entries.Count) VGPU-Mon PATH entries."
    }
    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $oldProcessPath = $env:Path
    try {
        $env:Path = "$userPath;$machinePath"
        $version = & vgpu --version
        if ($LASTEXITCODE -ne 0 -or $version -ne "VGPU-Mon $expectedVersion") {
            throw "Bootstrap-installed vgpu smoke test failed: $version"
        }
    }
    finally {
        $env:Path = $oldProcessPath
    }

    Invoke-HiddenProcess $uninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    $bootstrapInstalled = $false
}
finally {
    if ($bootstrapInstalled -and (Test-Path -LiteralPath $uninstaller)) {
        Invoke-HiddenProcess $uninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    }
    Remove-Item -LiteralPath $checksumPath -Force -ErrorAction SilentlyContinue
}

$afterBootstrap = Get-UserPathState
if ($afterBootstrap.Value -cne $before.Value -or $afterBootstrap.Kind -cne $before.Kind) {
    throw 'Bootstrap install/uninstall changed the original user PATH.'
}
if (Test-Path -LiteralPath $installDir) {
    throw 'Bootstrap uninstall left the application directory behind.'
}
if (Test-Path -LiteralPath 'HKCU:\Software\VGPU-Mon') {
    throw 'Bootstrap uninstall left VGPU-Mon installer state behind.'
}

Write-Host 'Installer, curl bootstrap, PATH command, upgrade, and uninstall tests passed.'
