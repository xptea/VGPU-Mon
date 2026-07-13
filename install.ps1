[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')]
    [string]$Repository = 'xptea/VGPU-Mon',

    [ValidatePattern('^v?[0-9]+\.[0-9]+\.[0-9]+$')]
    [string]$Version,

    [Parameter(DontShow)]
    [string]$InstallerPath,

    [Parameter(DontShow)]
    [string]$ChecksumPath
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Get-ExpectedChecksum {
    param(
        [Parameter(Mandatory)]
        [string]$ManifestPath,

        [Parameter(Mandatory)]
        [string]$FileName
    )

    $escapedName = [regex]::Escape($FileName)
    $pattern = "^([0-9A-Fa-f]{64})[ `t]+\*?$escapedName$"
    $hashes = @(
        foreach ($line in Get-Content -LiteralPath $ManifestPath) {
            $match = [regex]::Match($line, $pattern)
            if ($match.Success) {
                $match.Groups[1].Value.ToLowerInvariant()
            }
        }
    )
    if ($hashes.Count -ne 1) {
        throw "Expected exactly one SHA-256 entry for $FileName in SHA256SUMS.txt; found $($hashes.Count)."
    }
    return $hashes[0]
}

function Assert-FileChecksum {
    param(
        [Parameter(Mandatory)]
        [string]$FilePath,

        [Parameter(Mandatory)]
        [string]$ManifestPath
    )

    $fileName = Split-Path -Leaf $FilePath
    $expected = Get-ExpectedChecksum -ManifestPath $ManifestPath -FileName $fileName
    $actual = (Get-FileHash -LiteralPath $FilePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -cne $expected) {
        throw "SHA-256 verification failed for $fileName. Expected $expected but received $actual. The installer was not run."
    }
    Write-Host "Verified SHA-256: $actual"
}

function Get-ReleaseAsset {
    param(
        [Parameter(Mandatory)]
        [object[]]$Assets,

        [Parameter(Mandatory)]
        [string]$NamePattern,

        [Parameter(Mandatory)]
        [string]$Description
    )

    $matches = @($Assets | Where-Object { [string]$_.name -match $NamePattern })
    if ($matches.Count -ne 1) {
        throw "The GitHub release must contain exactly one $Description; found $($matches.Count)."
    }
    return $matches[0]
}

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'VGPU-Mon supports 64-bit Windows only.'
}
if (-not [Environment]::Is64BitOperatingSystem) {
    throw 'VGPU-Mon requires 64-bit Windows.'
}
if ([string]::IsNullOrWhiteSpace($InstallerPath) -xor [string]::IsNullOrWhiteSpace($ChecksumPath)) {
    throw 'InstallerPath and ChecksumPath must be supplied together.'
}

$temporaryDirectory = $null
try {
    if (-not [string]::IsNullOrWhiteSpace($InstallerPath)) {
        $installer = (Resolve-Path -LiteralPath $InstallerPath).Path
        $checksumManifest = (Resolve-Path -LiteralPath $ChecksumPath).Path
    }
    else {
        # Windows PowerShell 5.1 on older Windows 10 installations may not
        # negotiate TLS 1.2 by default. GitHub requires it.
        [Net.ServicePointManager]::SecurityProtocol =
            [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

        $headers = @{
            Accept                 = 'application/vnd.github+json'
            'User-Agent'           = 'VGPU-Mon-Installer'
            'X-GitHub-Api-Version' = '2022-11-28'
        }
        if ([string]::IsNullOrWhiteSpace($Version)) {
            $releaseUri = "https://api.github.com/repos/$Repository/releases/latest"
        }
        else {
            $tag = if ($Version.StartsWith('v')) { $Version } else { "v$Version" }
            $encodedTag = [Uri]::EscapeDataString($tag)
            $releaseUri = "https://api.github.com/repos/$Repository/releases/tags/$encodedTag"
        }

        Write-Host "Resolving VGPU-Mon release from $Repository..."
        try {
            $release = Invoke-RestMethod -Uri $releaseUri -Headers $headers -UseBasicParsing
        }
        catch {
            throw "Could not resolve the requested GitHub release: $($_.Exception.Message)"
        }

        $assets = @($release.assets)
        $installerAsset = Get-ReleaseAsset -Assets $assets `
            -NamePattern '^VGPU-Mon-[0-9]+\.[0-9]+\.[0-9]+-setup\.exe$' `
            -Description 'Windows setup executable'
        $checksumAsset = Get-ReleaseAsset -Assets $assets `
            -NamePattern '^SHA256SUMS\.txt$' `
            -Description 'SHA256SUMS.txt asset'

        foreach ($asset in @($installerAsset, $checksumAsset)) {
            $assetUri = [Uri][string]$asset.browser_download_url
            if ($assetUri.Scheme -cne 'https' -or $assetUri.Host -cne 'github.com') {
                throw "Release asset $($asset.name) has an unexpected download URL."
            }
        }

        $temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        $temporaryDirectory = [IO.Path]::GetFullPath(
            (Join-Path $temporaryBase "vgpu-mon-install-$([Guid]::NewGuid().ToString('N'))")
        )
        if (-not $temporaryDirectory.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing to create an installer directory outside the system temporary directory.'
        }
        [void](New-Item -ItemType Directory -Path $temporaryDirectory)

        $installer = Join-Path $temporaryDirectory ([string]$installerAsset.name)
        $checksumManifest = Join-Path $temporaryDirectory 'SHA256SUMS.txt'
        Write-Host "Downloading $($installerAsset.name)..."
        Invoke-WebRequest -Uri ([string]$installerAsset.browser_download_url) `
            -Headers $headers -UseBasicParsing -OutFile $installer
        Invoke-WebRequest -Uri ([string]$checksumAsset.browser_download_url) `
            -Headers $headers -UseBasicParsing -OutFile $checksumManifest
    }

    Assert-FileChecksum -FilePath $installer -ManifestPath $checksumManifest

    Write-Host 'Installing VGPU-Mon for the current user...'
    $process = Start-Process -FilePath $installer -ArgumentList @(
        '/VERYSILENT',
        '/SUPPRESSMSGBOXES',
        '/NORESTART',
        '/SP-'
    ) -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0) {
        throw "VGPU-Mon setup exited with code $($process.ExitCode)."
    }

    Write-Host ''
    Write-Host 'VGPU-Mon installed successfully.' -ForegroundColor Green
    Write-Host 'Open a new terminal, then run: vgpu'
}
finally {
    if ($null -ne $temporaryDirectory -and (Test-Path -LiteralPath $temporaryDirectory)) {
        $temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        $resolvedTemporaryDirectory = [IO.Path]::GetFullPath($temporaryDirectory)
        if ($resolvedTemporaryDirectory.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedTemporaryDirectory -Recurse -Force
        }
    }
}
