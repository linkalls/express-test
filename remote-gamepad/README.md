# Cemu Remote GamePad — native USB prototype

**Status: Experimental, untested on a real Android device and Windows GPU.**
This is a native extraction and touch proof of concept, not the finished
low-latency GamePad. Native GPU async capture / H.264 remain future work.

## Included

- Camera input: Cemu PR #1815
- TANK! TANK! TANK! GamePad fix: Cemu PR #2061
- Direct GamePad frame capture from the Cemu render path. No desktop capture.
- 127.0.0.1:49222 Windows socket intended for USB/ADB reverse only.
- Cemu internal VPAD touch injection; PC mouse and HD-6500 are unaffected.
- Android APK viewer source and GitHub Actions build.

## USB test procedure

1. On the feature branch, run Windows Actions:
   Build Cemu Camera + Tank + Native USB GamePad.
2. On the same branch, run Android Actions:
   Build Cemu Native USB GamePad Android APK.
3. Install the APK on your Android phone and enable USB debugging.
4. Connect to a trusted Windows PC by USB and run:
       adb devices
       adb reverse tcp:49222 tcp:49222
5. Launch the modified Cemu, start a game that outputs GamePad frames,
   then open the Android app and press Connect.
6. Touch the image: Android coordinates are inserted directly into VPAD.
   The Cemu server only starts when a DRC frame is produced.

## Limitations

- Stock Vulkan screenshot readback BLOCKS on a GPU synchronization point.
  Native source-frame preview is throttled to at most eight frames per
  second to avoid making this much worse. It may STILL stutter.
- Raw RGB24 over TCP: about 1.23 MB per 854x480 frame.
  This is a functional scaffold rather than a usable 60fps codec pipeline.
- USB loopback only. The server has no pairing or TLS; NEVER expose its
  listener to the public network / 0.0.0.0.
- Frame capture is prior to some GamePad overlays; software keyboard
  and pop-ups may be missing.
- The existing manual Cemu screenshot feature shares the screenshot
  request slot. Do not trigger manual screenshots during streaming.
- DSU, remote camera/microphone, audio, gyro and rumble are NOT implemented.
- Single client only. No games, keys or copyrighted game assets included.

## Required production work

1. Async ring-buffered Vulkan and OpenGL GPU readback with fences.
2. Native H.264 hardware encoder, Android MediaCodec Surface decoder.
3. Pairing, TLS/authentication and Wi-Fi LAN discovery.
4. DSU/controller, microphone/camera, audio and rumble.
5. Controller and frame latency diagnostics and regressions.

## Integration

Windows CI checks out pinned Camera PR #1815, cherry-picks pinned Tank
PR #2061, then injects the new bridge header and Cemu touch/render hooks.
The existing combined PR is left intact; everything stays on a feature
branch until validated.
