[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$OutputName = 'vgpu-mon'
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
    & "$root\build.ps1" -Configuration Release -Test -OutputName $OutputName
}

$executable = Join-Path $root "build\$OutputName.exe"
if (-not (Test-Path -LiteralPath $executable)) {
    throw "Release executable was not found at $executable."
}

$dist = Join-Path $root 'dist'
$packageDir = Join-Path $dist "VGPU-Mon-$version-windows-x64"
$archive = Join-Path $dist "VGPU-Mon-$version-windows-x64.zip"

if (Test-Path -LiteralPath $packageDir) { Remove-Item -LiteralPath $packageDir -Recurse -Force }
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null

Copy-Item -LiteralPath $executable -Destination (Join-Path $packageDir 'vgpu-mon.exe')
Copy-Item -LiteralPath "$root\README.md" -Destination $packageDir
Copy-Item -LiteralPath "$root\LICENSE" -Destination $packageDir
Copy-Item -LiteralPath "$root\CHANGELOG.md" -Destination $packageDir
Copy-Item -LiteralPath "$root\SECURITY.md" -Destination $packageDir
Compress-Archive -Path "$packageDir\*" -DestinationPath $archive -CompressionLevel Optimal

Write-Host "Packaged $archive"
