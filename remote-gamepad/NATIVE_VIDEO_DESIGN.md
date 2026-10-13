# Cemu-native Remote GamePad architecture (target implementation)

## Non-negotiable requirements

1. No desktop/window/screen recorder. The source is the Cemu DRC texture,
   at the render-target scan-buffer path.
2. Preserve the existing Cemu Camera PR #1815 and Tank PR #2061.
3. No blocking GPU wait on the emulation/render thread for video export.
4. TV display and DRC output are independent. DRC works with its window hidden.
5. Wi-Fi and USB use the same encrypted, authenticated session protocol.
6. Never let video backpressure pause CPU emulation or GPU submissions.
7. Touch goes directly to VPADController, independent of PC window focus.
8. At minimum the PC keyboard/controller and remote touch can coexist.

## Native interception point (confirmed from Cemu source)

Cemu: src/Cafe/HW/Latte/Core/LatteRenderTarget.cpp

- LatteRenderTarget_itHLECopyColorBufferToScanBuffer(...)
- LatteTC_GetTextureSliceViewOrTryCreate(...)
- renderTarget & RENDER_TARGET_DRC
- LatteRenderTarget_copyToBackbuffer(..., true)

Tap the texView before the WindowSystem::IsPadWindowOpen gate and add an
independent native frame-producer interface:
  DrcFrameProducer::Submit(LatteTextureView*, frame_id, emulated_timestamp)

Do not treat GamePad overlays as window-system content. If software keyboard
and overlay support are required, explicitly composite them into the
exported image or surface. Make overlay priority/game visibility configurable.

## Vulkan producer

Cemu: src/Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.cpp

Existing HandleScreenshotRequest() is intentionally **not** suitable for video:
it submits and waits with WaitCommandBufferFinished and maps memory per frame.

Production algorithm:

- Allocate ring of 3-4 persistent, host-visible staging buffers sized for
  DRC resolution/pixel format, respecting VkPhysicalDeviceLimits and alignment.
- Prefer GPU color conversion/reformat to NV12 where the encoder requires it.
- Record transitions and vkCmdCopyImageToBuffer on Cemu's rendering queue.
- Associate submission with a fence or timeline semaphore value. NEVER wait
  on that fence from the game/render thread.
- Poll completions on a separate worker; map host-visible memory only when
  the corresponding GPU work completed.
- Immediately restore the source image layout and access flags.
- If all ring slots are occupied: drop the **newest capture request** or
  replace the oldest unsubmitted request, without blocking Cemu.
- Validate image format, mip, layer, pitch, color space and native tex size.
- Preserve all existing screenshots, rendering, Camera, and Tank functionality.

The queue synchronization must be integrated with Cemu's command-buffer
lifetime, avoiding reuse of staging slots before the GPU has finished.

## OpenGL producer

Cemu: src/Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.cpp

Existing HandleScreenshotRequest() uses synchronous glGetTexImage.
For the production producer use:
- Pixel Buffer Objects (PBO), ring size >=3.
- Asynchronous texture readback (glGetTextureSubImage into bound PBO).
- GLsync fences; poll with zero timeout instead of glFinish.
- Correct row packing, source orientation, color conversion.
- Bound memory allocations and per-frame CPU use.

## Encoder + transport

Separate encoder worker from Cemu render thread. Prefer hardware H.264:
- Windows Media Foundation hardware encoder MFT (when supported).
- GPU-native interop, typically DXGI/D3D11 texture path, where feasible.
- Encode baseline/main with low-latency settings; no B-frames.
- DRC resolution 854x480 unless a game/graphic pack produces otherwise.
- Video adaptive bitrate (initial 4-8Mbps), nominal max 60fps, zero
  unnecessary queueing; downgrade 30fps if hardware cannot sustain.
- Encode and send only the newest drawable frame under backpressure.

Media Foundation H264 MFT accepts formats including NV12. Avoid claiming
zero-copy from a Vulkan texture into MF without explicitly implementing
interop and testing the GPU drivers. Vulkan -> D3D11 external-memory sharing
is not universally available.

Transport:
- Local-network discovery via NSD/mDNS when paired.
- ADB reverse USB path: Android -> localhost:49222 -> Cemu localhost.
- USB tethering fallback can use the same TLS endpoint over an IP network
  but must be authenticated and explicitly enabled.
- Use TLS 1.3 / QUIC or an equivalent proven authenticated transport
  once leaving loopback. No homegrown crypto.
- Separate media and reliable control channels.
- Android app holds a pinned server public key established on first pair.
- Frame IDs, timestamps and explicit discontinuity markers.
- Low-latency codec-specific configuration upon every reconnect.

## Android

- Kotlin + Android MediaCodec with Surface output for hardware decoding.
- Crop/letterbox touch mapping independent of device resolution.
- MediaCodec low-latency setting where supported.
- Maintain one active session, release touch/buttons on disconnect.
- USB session does not need a Wi-Fi network: adb reverse requires developer
  USB debugging and authorization.
- For Wi-Fi on targetSdk 37+: follow Android 17 ACCESS_LOCAL_NETWORK rules
  or system-mediated NSD picker. Do not silently fail due to permission.

## Direct controller input

Cemu: src/input/emulated/VPADController.cpp / update_touch()
- Atomically snapshot remote touch state; normalised [0,1].
- Convert into VPAD calibrated raw touch units (x ~92..3975,
  y ~3841..147) respecting native coordinate direction.
- Protect against stuck touch on packet loss and session death.
- DSU or direct emulated VPAD for buttons/gyroscope.
- Never steal the Windows cursor or focus.

## Quality gates

- End-to-end frame integrity at 854x480.
- Android touch alignment at all 4 corners and center.
- USB unplug while touching: released within 150ms after connection loss.
- Reconnect within a few seconds after restoring USB, no Cemu restart.
- No video export crash if software renderer/GamePad window is closed.
- No sustained FPS regression >5% versus no-stream baseline on the test PC
  at the same renderer/resolution (measured, not assumed).
- 60fps on supported game/GPU; no claim of guaranteed 60fps for 30fps titles.
- Gamepad audio, gyro, camera and mic each have independent regression tests.

## Current preview vs production

Current branch is a correctness prototype:
native texture -> screenshot readback -> RGB24 TCP loopback -> Android Bitmap.
It is intentionally capped at ~8fps and may stall Vulkan's GPU. **Do not**
mistake its existence for production low-latency H.264 implementation.

Roadmap: native async producer first, hardware encode/decode second, then
authenticated Wi-Fi and peripherals. Maintain the Camera + Tank PR pins.
