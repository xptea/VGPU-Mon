[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

Push-Location $root
try {
    & (Join-Path $root 'analyze.cmd')
    if ($LASTEXITCODE -ne 0) {
        throw "Static analysis failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}
