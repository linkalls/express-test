# H264 GamePad over USB. Android app connects to localhost:49224 using TLS.
# The Windows server listens ONLY on 127.0.0.1 in this mode.
param(
    [ValidateSet("h264_amf", "h264_nvenc", "h264_qsv", "libx264")]
    [string]$Encoder = "h264_amf",
    [string]$Ffmpeg = "ffmpeg.exe",
    [string]$Cemu = ".\Cemu.exe"
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path -LiteralPath $Cemu)) { throw "Cemu executable not found: $Cemu" }
& adb devices
if ($LASTEXITCODE -ne 0) { throw "ADB failed. Install Android Platform-Tools." }
& adb reverse tcp:49224 tcp:49224
if ($LASTEXITCODE -ne 0) { throw "USB debugging not authorized, or adb reverse failed." }
$encoders = & $Ffmpeg -hide_banner -encoders 2>&1
if ($LASTEXITCODE -ne 0 -or -not ($encoders | Select-String -Quiet -Pattern "\b$Encoder\b")) {
    throw "Missing FFmpeg encoder: $Encoder"
}
$env:CEMU_REMOTE_H264_USB = "1"
$env:CEMU_REMOTE_WIFI = "0"  # Do NOT expose a LAN listener in USB-only mode.
$env:CEMU_REMOTE_H264_ENCODER = $Encoder
$env:CEMU_REMOTE_FFMPEG = (Get-Command $Ffmpeg -ErrorAction Stop).Source
Start-Process -FilePath (Resolve-Path -LiteralPath $Cemu).Path | Out-Null
Write-Host "Cemu H264 via ADB reverse enabled: Android localhost TCP 49224" -ForegroundColor Green
Write-Host "On Android, use H264 TLS connection, IP 127.0.0.1."
Write-Host "Read SHA256 pin and token after a GamePad game starts:"
Write-Host "  $env:LOCALAPPDATA\CemuRemoteGamePad\wifi-fingerprint.txt"
Write-Host "  $env:LOCALAPPDATA\CemuRemoteGamePad\wifi-access-token.txt"
