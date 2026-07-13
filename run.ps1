[CmdletBinding()]
param(
    [int]$Gpu = 0,
    [ValidateRange(250, 5000)]
    [int]$Interval = 1000,
    [ValidateSet('', 'gpu', 'vram', 'memory-controller', '3d', 'copy', 'decode', 'encode', 'compute', 'temperature', 'power')]
    [string]$Chart = '',
    [switch]$Demo
)

$ErrorActionPreference = 'Stop'
$executable = Join-Path $PSScriptRoot 'build\vgpu-mon.exe'
if (-not (Test-Path -LiteralPath $executable)) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release
}
$arguments = @('--gpu', $Gpu, '--interval', $Interval)
if ($Demo) { $arguments += '--demo' }
if ($Chart) { $arguments += @('--chart', $Chart) }
& $executable @arguments
exit $LASTEXITCODE
