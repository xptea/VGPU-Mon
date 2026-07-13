[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'Sanitize')]
    [string]$Configuration = 'Release',
    [switch]$Test,
    [ValidatePattern('^[A-Za-z0-9._-]+$')]
    [string]$OutputName = 'vgpu-mon'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$testArgument = if ($Test) { 'test' } else { 'notest' }

Push-Location $root
try {
    & "$root\build.cmd" $Configuration $testArgument $OutputName
    if ($LASTEXITCODE -ne 0) {
        throw "Native build failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}
