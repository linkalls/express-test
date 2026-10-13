# Connect a USB Android device to the local Cemu-native GamePad bridge.
# Run in PowerShell after installing Android Platform-Tools.
# USB debugging must be enabled and this PC must be authorized on the phone.
param(
    [string]$Adb = "adb",
    [string]$Serial = "",
    [int]$Port = 49222
)

$ErrorActionPreference = "Stop"

try {
    & $Adb version | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "ADB executable returned an error."
    }
} catch {
    throw "adb not found. Install Android SDK Platform-Tools, then add adb to PATH."
}

& $Adb start-server | Out-Null
$devices = @(& $Adb devices)
$authorized = @(
    $devices | Where-Object {
        $_ -match '^([^\s]+)\s+device$'
    } | ForEach-Object {
        ($_.Trim() -split '\s+')[0]
    }
)

if ($Serial -ne "") {
    if ($Serial -notin $authorized) {
        throw "Requested device '$Serial' is not authorized. Check the USB-debugging dialog."
    }
    $selected = $Serial
} else {
    if ($authorized.Count -eq 0) {
        throw "No authorized Android device found. Enable USB debugging and accept the computer's RSA key."
    }
    if ($authorized.Count -gt 1) {
        throw "Multiple devices found: $($authorized -join ', '). Rerun with -Serial DEVICE_ID."
    }
    $selected = $authorized[0]
}

$rule = "tcp:$Port"
& $Adb -s $selected reverse $rule $rule
if ($LASTEXITCODE -ne 0) {
    throw "ADB reverse failed. Verify that the device supports adb reverse."
}

Write-Host "USB forwarding ready: Android 127.0.0.1:$Port -> Windows 127.0.0.1:$Port" -ForegroundColor Green
Write-Host "Device: $selected"
Write-Host "Start patched Cemu + a GamePad game, then tap Connect in the Android app."
