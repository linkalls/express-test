#!/usr/bin/env python3
"""Apply the USB GamePad proof-of-concept to the pinned combined Cemu tree.

Fail closed if upstream moves a patch anchor. We intentionally do NOT modify
Cemu's camera path or the TANK! TANK! TANK! LatteMRT fix. The native bridge
hooks the existing DRC texture before any desktop window capture is involved.
"""
from pathlib import Path
import shutil
import sys

def replace_once(text: str, old: str, new: str, filename: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{filename}: expected one anchor, found {count}: {old[:100]!r}")
    return text.replace(old, new, 1)

def patch_render_target(cemu: Path) -> None:
    target = cemu / "src/Cafe/HW/Latte/Core/LatteRenderTarget.cpp"
    original = target.read_text(encoding="utf-8")
    if '#include "remote/RemoteGamePadBridge.h"' in original:
        raise RuntimeError("Native GamePad patch already applied to LatteRenderTarget.cpp")
    source = '#include "remote/RemoteGamePadBridge.h"\n' + original
    anchor = "\tif ((renderTarget & RENDER_TARGET_DRC) && g_renderer->IsPadWindowActive())\n"
    insertion = """#ifdef _WIN32
    // The remote preview reads the GamePad source texture directly from Cemu.
    // The Windows desktop and GamePad window are never captured.
    if (renderTarget & RENDER_TARGET_DRC)
    {
        RemoteGamePad::Start();
        if (RemoteGamePad::Connected() || RemoteGamePadVideo::Connected())
        {
            // Vulkan's legacy screenshot path still uses a blocking GPU wait,
            // so it remains at 8fps until the Vulkan async pipeline is added.
            // OpenGL has its own bounded 60Hz PBO+fence producer instead.
            if (g_renderer->GetType() != RendererAPI::OpenGL || RemoteGamePad::Connected())
                RemoteGamePad::RequestNativeFrameIfDue(g_renderer.get());

            // Cemu ordinarily processes the DRC screenshot request from
            // LatteRenderTarget_copyToBackbuffer() when the pad window is open.
            // When it is closed, read the same native source texture directly.
            if (!g_renderer->IsPadWindowActive())
            {
                LatteTexture_UpdateDataToLatest(texView->baseTexture);
                LatteTC_MarkTextureStillInUse(texView->baseTexture);
                g_renderer->HandleScreenshotRequest(texView, true);
            }
        }
    }
#endif

"""
    source = replace_once(source, anchor, insertion + anchor, str(target))
    target.write_text(source, encoding="utf-8")

def patch_touch(cemu: Path) -> None:
    target = cemu / "src/input/emulated/VPADController.cpp"
    original = target.read_text(encoding="utf-8")
    if '#include "remote/RemoteGamePadBridge.h"' in original:
        raise RuntimeError("Native GamePad patch already applied to VPADController.cpp")
    source = '#include "remote/RemoteGamePadBridge.h"\n' + original
    # This block occurs inside VPADController::update_touch.
    anchor = "\tauto& instance = InputManager::instance();\n\tbool pad_view;\n"
    injection = """#ifdef _WIN32
    // Remote touchscreen coordinates go straight into emulated VPAD input.
    // No Windows mouse events, focus stealing or fake desktop touches.
    float remoteX = 0.0f;
    float remoteY = 0.0f;
    if (RemoteGamePad::GetPressedTouch(remoteX, remoteY))
    {
        status.tpData.touch = kTpTouchOn;
        status.tpData.validity = kTpValid;
        status.tpData.x = static_cast<uint16>(remoteX * 3883.0f + 92.0f);
        status.tpData.y = static_cast<uint16>(4095.0f - remoteY * 3694.0f - 254.0f);
        m_last_touch_position = glm::ivec2{status.tpData.x, status.tpData.y};
        return;
    }
#endif

"""
    source = replace_once(source, anchor, injection + anchor, str(target))
    target.write_text(source, encoding="utf-8")

def patch_opengl(cemu: Path) -> None:
    """Direct non-blocking PBO capture is inserted into native GL backend."""
    target = cemu / "src/Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.cpp"
    source = target.read_text(encoding="utf-8")
    if 'remote/RemoteGamePadAsyncGL.inc' in source:
        raise RuntimeError("OpenGL async capture overlay already applied")
    source = replace_once(
        source,
        '#include "config/LaunchSettings.h"',
        '#include "config/LaunchSettings.h"\n#include "remote/RemoteGamePadAsyncGL.inc"',
        str(target),
    )
    anchor = "void OpenGLRenderer::HandleScreenshotRequest(LatteTextureView* texView, bool padView)\n{\n"
    injected = (
        "    // Independent native DRC streaming: never waits for a GL readback.\n"
        "    RemoteGamePadAsyncGL::Capture(texView, padView);\n"
    )
    source = replace_once(source, anchor, anchor + injected, str(target))
    target.write_text(source, encoding="utf-8")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("Usage: apply_native_bridge.py <Cemu source root> <bridge header>")
    cemu = Path(sys.argv[1]).resolve()
    header = Path(sys.argv[2]).resolve()
    if not header.is_file():
        raise FileNotFoundError(header)
    dest = cemu / "src/remote/RemoteGamePadBridge.h"
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(header, dest)
    for extra in ("RemoteGamePadVideo.h", "RemoteGamePadAsyncGL.inc"):
        sibling = header.parent / extra
        if not sibling.is_file():
            raise FileNotFoundError(sibling)
        shutil.copyfile(sibling, dest.parent / extra)
    patch_render_target(cemu)
    patch_touch(cemu)
    patch_opengl(cemu)
    print("Native GamePad USB bridge injected into Cemu DRC and VPAD code.")

if __name__ == "__main__":
    main()
