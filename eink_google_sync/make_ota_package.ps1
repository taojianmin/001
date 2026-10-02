param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot "build_serial_wifi_setup_v1"),
    [string]$FirmwareVersion = "2.6.1-serial-wifi-setup"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$sourceBinary = Join-Path $BuildDirectory "eink_google_sync.ino.bin"
if (-not (Test-Path -LiteralPath $sourceBinary)) {
    throw "Firmware binary not found: $sourceBinary"
}

$releaseDirectory = Join-Path $PSScriptRoot "release"
New-Item -ItemType Directory -Path $releaseDirectory -Force | Out-Null

$releaseName = "eink_calendar_$FirmwareVersion.bin"
$releaseBinary = Join-Path $releaseDirectory $releaseName
$checksumFile = "$releaseBinary.sha256"

Copy-Item -LiteralPath $sourceBinary -Destination $releaseBinary -Force
$checksum = (Get-FileHash -LiteralPath $releaseBinary -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath $checksumFile -Value "$checksum  $releaseName" -Encoding ascii

[pscustomobject]@{
    Firmware = $releaseBinary
    Checksum = $checksumFile
    Sha256 = $checksum
    Bytes = (Get-Item -LiteralPath $releaseBinary).Length
}
