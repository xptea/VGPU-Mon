[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root 'dist'
$versionHeader = Get-Content -LiteralPath (Join-Path $root 'src\version.h') -Raw
$versionMatch = [regex]::Match(
    $versionHeader,
    '#define\s+VGPU_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"'
)
if (-not $versionMatch.Success) {
    throw 'Could not read VGPU_VERSION from src\version.h.'
}
$version = $versionMatch.Groups[1].Value
$setup = Join-Path $dist "VGPU-Mon-$version-setup.exe"
$archive = Join-Path $dist "VGPU-Mon-$version-windows-x64.zip"
foreach ($required in @($setup, $archive, (Join-Path $root 'install.ps1'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required release asset was not found: $required"
    }
}

$bootstrap = Join-Path $dist 'install.ps1'
$versionManifest = Join-Path $dist 'version.txt'
$checksumManifest = Join-Path $dist 'SHA256SUMS.txt'
Copy-Item -LiteralPath (Join-Path $root 'install.ps1') -Destination $bootstrap -Force

$setupHash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
@(
    "version=$version"
    "installer=$(Split-Path -Leaf $setup)"
    "sha256=$setupHash"
) | Set-Content -LiteralPath $versionManifest -Encoding ascii

$assets = @($archive, $setup, $bootstrap, $versionManifest) |
    Sort-Object { Split-Path -Leaf $_ }
$checksumLines = foreach ($asset in $assets) {
    $hash = (Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $(Split-Path -Leaf $asset)"
}
$checksumLines | Set-Content -LiteralPath $checksumManifest -Encoding ascii

Write-Host "Staged release metadata for VGPU-Mon $version."
