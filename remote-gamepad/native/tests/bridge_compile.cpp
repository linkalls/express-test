// Compile-only smoke test, using a stub for the Cemu Renderer interface.
#include "RemoteGamePadBridge.h"
#include <type_traits>

static_assert(std::is_same_v<
    decltype(RemoteGamePad::Connected()), bool>);

int main()
{
    Renderer renderer;
    RemoteGamePad::RequestNativeFrameIfDue(&renderer);
    float x = 0.0f;
    float y = 0.0f;
    RemoteGamePad::GetPressedTouch(x, y);
    return RemoteGamePad::Connected() ? 0 : 0;
}
