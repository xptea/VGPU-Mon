[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$version = '6.7.3'
$expectedSha256 = '9c73c3bae7ed48d44112a0f48e66742c00090bdb5bef71d9d3c056c66e97b732'
$candidates = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
)

if ($candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1) {
    Write-Host 'Inno Setup is already installed.'
    return
}

$download = Join-Path ([System.IO.Path]::GetTempPath()) "innosetup-$version.exe"
$url = "https://files.jrsoftware.org/is/6/innosetup-$version.exe"
try {
    Invoke-WebRequest -Uri $url -OutFile $download -UseBasicParsing
    $actualSha256 = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualSha256 -ne $expectedSha256) {
        throw "Inno Setup download hash mismatch. Expected $expectedSha256, got $actualSha256."
    }

    $process = Start-Process -FilePath $download -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER'
    ) -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0) {
        throw "Inno Setup installer exited with code $($process.ExitCode)."
    }
}
finally {
    Remove-Item -LiteralPath $download -Force -ErrorAction SilentlyContinue
}

if (-not ($candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1)) {
    throw 'Inno Setup installation completed but ISCC.exe was not found.'
}
Write-Host "Installed Inno Setup $version."
