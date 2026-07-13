[CmdletBinding()]
param(
    [string]$Executable = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\vgpu-mon.exe')
)

$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Executable).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    throw 'Visual Studio C++ build tools were not found.'
}
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

function Invoke-Dumpbin {
    param([string]$Arguments)
    $command = 'call "' + $vcvars + '" >nul && dumpbin ' + $Arguments + ' "' + $exe + '"'
    $output = & $env:ComSpec /d /s /c $command 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed: $output"
    }
    return $output -join "`n"
}

$headers = Invoke-Dumpbin '/headers'
$requiredMitigations = @(
    'High Entropy Virtual Addresses',
    'Dynamic base',
    'NX compatible',
    'Control Flow Guard',
    'CET compatible'
)
foreach ($mitigation in $requiredMitigations) {
    if ($headers -notmatch [regex]::Escape($mitigation)) {
        throw "Release executable is missing PE mitigation: $mitigation"
    }
}

$dependents = Invoke-Dumpbin '/dependents'
$runtimeDependencies = [regex]::Matches(
    $dependents,
    '(?im)^\s+(vcruntime|msvcp|ucrtbase)[^\r\n]*\.dll\s*$'
)
if ($runtimeDependencies.Count -ne 0) {
    throw "Release executable unexpectedly depends on a dynamic MSVC runtime: $($runtimeDependencies.Value -join ', ')"
}

Write-Host 'PE mitigations and static MSVC runtime checks passed.'
