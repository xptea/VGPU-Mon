[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Executable
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = (Resolve-Path -LiteralPath (Join-Path $root $Executable)).Path
$versionHeader = Get-Content -LiteralPath (Join-Path $root 'src\version.h') -Raw
$versionMatch = [regex]::Match($versionHeader, '#define\s+VGPU_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"')
if (-not $versionMatch.Success) {
    throw 'Could not read VGPU_VERSION from src\version.h.'
}
$expectedVersion = $versionMatch.Groups[1].Value

function Invoke-ExpectedExit {
    param(
        [int]$ExpectedExit,
        [string[]]$Arguments
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $exe
    $startInfo.Arguments = ($Arguments | ForEach-Object {
        if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }) -join ' '
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($startInfo)
    $stdout = $process.StandardOutput.ReadToEnd()
    $stderr = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    $actualExit = $process.ExitCode
    if ($actualExit -ne $ExpectedExit) {
        throw "Expected exit $ExpectedExit for '$($Arguments -join ' ')', got $actualExit. Output: $stdout $stderr"
    }
    return @($stdout, $stderr | Where-Object { $_ })
}

$version = (Invoke-ExpectedExit 0 @('--version')) -join "`n"
if ($version.Trim() -ne "VGPU-Mon $expectedVersion") {
    throw "Unexpected version output: $version"
}

$jsonText = (Invoke-ExpectedExit 0 @('--demo', '--interval', '250', '--json')) -join "`n"
$snapshot = $jsonText | ConvertFrom-Json
if ($snapshot.name -ne 'VGPU-Mon Demo GPU') {
    throw "Unexpected demo GPU name: $($snapshot.name)"
}
if ($snapshot.memory_total_bytes -le 0 -or $snapshot.memory_used_bytes -le 0) {
    throw 'Demo JSON did not include valid memory telemetry.'
}
if (@($snapshot.processes).Count -lt 6) {
    throw 'Demo JSON did not include the expected process rows.'
}
if (-not $snapshot.engines.'3d_percent' -or -not $snapshot.engines.copy_percent) {
    throw 'Demo JSON did not include engine telemetry.'
}

[void](Invoke-ExpectedExit 2 @('--interval', '100', '--once'))
[void](Invoke-ExpectedExit 2 @('--gpu', 'nope', '--once'))
[void](Invoke-ExpectedExit 2 @('--demo', '--chart', '--json'))

Write-Host 'CLI version, demo JSON, and validation tests passed.'
