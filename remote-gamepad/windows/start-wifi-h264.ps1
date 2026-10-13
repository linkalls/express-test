# Run a patched Cemu with opt-in encrypted local Wi-Fi GamePad streaming.
# A Windows FFmpeg build with the AMD/NVIDIA/Intel H264 encoder is required.
# Use only your trusted private LAN. The listener uses TLS1.3 and fingerprint pinning.
param(
    [ValidateSet("h264_amf", "h264_nvenc", "h264_qsv", "libx264")]
    [string]$Encoder = "h264_amf",
    [string]$Ffmpeg = "ffmpeg.exe",
    [string]$Cemu = ".\Cemu.exe"
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $Cemu)) {
    throw "Cemu executable not found: $Cemu"
}
try {
    & $Ffmpeg -hide_banner -encoders | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "FFmpeg did not launch."
    }
} catch {
    throw "FFmpeg not available. Install an FFmpeg build supporting $Encoder, then use -Ffmpeg full-path."
}
$available = & $Ffmpeg -hide_banner -encoders 2>&1
if (-not ($available | Select-String -Quiet -Pattern "\b$Encoder\b")) {
    throw "The selected FFmpeg build does not expose encoder $Encoder."
}
$env:CEMU_REMOTE_WIFI = "1"
$env:CEMU_REMOTE_H264_ENCODER = $Encoder
$env:CEMU_REMOTE_FFMPEG = (Get-Command $Ffmpeg -ErrorAction Stop).Source
Write-Host "Cemu TLS Wi-Fi GamePad enabled on TCP/49224 (certificate pin REQUIRED)." -ForegroundColor Cyan
Write-Host "Selected H.264 encoder: $Encoder"
Write-Host "Windows Firewall must allow Cemu.exe TCP 49224 on the PRIVATE network only."
Write-Host "Play a game which renders to the Wii U GamePad."
Start-Process -FilePath (Resolve-Path -LiteralPath $Cemu).Path -PassThru | Out-Null

$pinPath = Join-Path $env:LOCALAPPDATA "CemuRemoteGamePad\wifi-fingerprint.txt"
if (Test-Path -LiteralPath $pinPath) {
    Write-Host "The Android app MUST use this exact TLS certificate fingerprint:"
    Get-Content -LiteralPath $pinPath
} else {
    Write-Host "When the game starts, read certificate pin from: $pinPath"
}
