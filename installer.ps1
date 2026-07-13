[CmdletBinding()]
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$versionHeader = Get-Content -LiteralPath (Join-Path $root 'src\version.h') -Raw
$versionMatch = [regex]::Match($versionHeader, '#define\s+VGPU_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"')
if (-not $versionMatch.Success) {
    throw 'Could not read VGPU_VERSION from src\version.h.'
}
$version = $versionMatch.Groups[1].Value

if (-not $SkipBuild) {
    & (Join-Path $root 'build.ps1') -Configuration Release -Test
}

$compilerCandidates = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
)
$compiler = $compilerCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $compiler) {
    throw 'Inno Setup 6 is required. Install it with: winget install --id JRSoftware.InnoSetup --exact --scope user'
}

Push-Location $root
try {
    & $compiler "/DMyAppVersion=$version" (Join-Path $root 'installer.iss')
    if ($LASTEXITCODE -ne 0) {
        throw "Installer compilation failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}

$installer = Join-Path $root "dist\VGPU-Mon-$version-setup.exe"
if (-not (Test-Path -LiteralPath $installer)) {
    throw "Installer compiler did not produce $installer."
}
Write-Host "Built $installer"
