# Cemu Remote GamePad — 60Hz / H264 / TLS Wi-Fi (EXPERIMENTAL)

Based on the pinned Camera PR #1815 + Tank PR #2061 combination.

## Added
- OpenGL: direct native DRC texture capture, four pixel-pack buffers, GLsync zero-timeout GPU polling, about 60Hz schedule, stale-frame dropping.
- Hardware H264: FFmpeg child process with selectable h264_amf (AMD), h264_nvenc (NVIDIA), or h264_qsv (Intel). Requires FFmpeg executable, matching driver and encoder. Optional libx264 only if selected explicitly. FFmpeg is not bundled.
- Wi-Fi: TLS1.3, manual certificate SHA256 pinning, opt-in via CEMU_REMOTE_WIFI=1; TCP port 49224. Pin stored at %LOCALAPPDATA%\CemuRemoteGamePad\wifi-fingerprint.txt.
- Android: Wi-Fi host/IP and fingerprint fields, pinned SSLSocket, MediaCodec Surface H264 decode and touch return; original USB preview remains.

## Usage
1. Install ffmpeg.exe with your GPU's H264 encoder.
2. Run the Windows startup script: powershell -ExecutionPolicy Bypass -File .\start-wifi-h264.ps1 -Encoder h264_amf -Cemu .\Cemu.exe
3. Allow inbound port TCP 49224 ONLY on Windows Private networks.
4. Launch a GamePad-capable game, then read the SHA256 fingerprint under LOCALAPPDATA/CemuRemoteGamePad.
5. On Android, enter PC LAN IPv4 and the full 64-character fingerprint. Tap Wi-Fi TLSで接続.
6. Validate decoding, touch points and performance using actual hardware.

## Critical limitations
- Vulkan still uses the former screenshot path, capped at approximately 8fps. The Vulkan asynchronous GPU readback implementation remains incomplete. 60Hz targeting applies to OpenGL only.
- OpenGL scheduling at ~60Hz is not a verified 60fps pipeline; hardware encoder, frame conversion, game fps and network all limit achieved output.
- FFmpeg hardware encoding uses RGB CPU copies and FFmpeg conversion to NV12; NOT zero-copy. A later Media Foundation/D3D11 path is preferable for throughput.
- TLS credentials are stored in a user-profile file and rely on OS filesystem permissions. Keep the private key secret. Pin mismatch must fail closed.
- No Wi-Fi service discovery, automatic QR pairing or adaptive congestion control yet.
- End-to-end H264 frame parsing and connection must be verified on Ryzen Windows PC + Pixel Android; Actions passing does not establish runtime success.
- Wi-Fi is disabled by default. Never expose it on a public Internet interface; Windows Firewall Private profile only.
- Camera and Tank changes are preserved; remote audio/microphone/gyro not yet integrated.

## Validation
- Android APK compiles and uploads.
- Windows Camera + Tank + Wi-Fi H264 patch compiles, links, and uploads.
- Real hardware runtime TLS authentication + video + touch regression tests.
- Measure frame pacing, p50/p95 latency, and GPU/CPU overhead; do not assert 60fps until measured.
