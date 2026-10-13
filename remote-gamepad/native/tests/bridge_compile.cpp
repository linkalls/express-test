// Compile and run the native GamePad bridge without the full Cemu tree.
// The mock video backend simulates authenticated Wi-Fi transport, while the
// renderer stub checks screenshot callback compatibility.
#define REMOTE_GAMEPAD_VIDEO_MOCK 1
#include "RemoteGamePadBridge.h"
#include <type_traits>
#include <cmath>
#include <vector>

static_assert(std::is_same_v<decltype(RemoteGamePad::Connected()), bool>);

int main()
{
    Renderer renderer;
    // The screenshot callback must remain compatible with Cemu's renderer.
    RemoteGamePad::RequestNativeFrameIfDue(&renderer);

    float x = 0.0f;
    float y = 0.0f;
    if (RemoteGamePad::GetPressedTouch(x, y))
        return 1; // Never accept an unauthenticated source.

    // Wi-Fi-only: no USB client is connected. The authenticated TLS video
    // backend must still make GamePad touch available to the VPAD layer.
    RemoteGamePadVideo::mockConnected = true;

    // Simulate the first fragment of a network touch message.
    std::vector<uint8_t> controls{'T', 'C', 'H'};
    RemoteGamePad::detail::ProcessTouchPackets(controls);
    if (RemoteGamePad::GetPressedTouch(x, y))
        return 2; // Incomplete packets must not update touch state.

    // Finish TCH1 + x(0x8000) + y(0x7fff) + pressed(1).
    controls.insert(controls.end(), {'1', 0x00, 0x80, 0xff, 0x7f, 0x01});
    RemoteGamePad::detail::ProcessTouchPackets(controls);
    if (!controls.empty() || !RemoteGamePad::GetPressedTouch(x, y))
        return 3;
    if (std::fabs(x - (32768.0f / 65535.0f)) > 0.0001f ||
        std::fabs(y - (32767.0f / 65535.0f)) > 0.0001f)
        return 4;

    // Releasing the contact must immediately remove the pressed state.
    std::vector<uint8_t> release{'T', 'C', 'H', '1', 0, 0, 0, 0, 0};
    RemoteGamePad::detail::ProcessTouchPackets(release);
    if (RemoteGamePad::GetPressedTouch(x, y))
        return 5;

    // Network disconnection must invalidate any previously pressed state.
    std::vector<uint8_t> down{'T', 'C', 'H', '1', 0, 0, 0, 0, 1};
    RemoteGamePad::detail::ProcessTouchPackets(down);
    RemoteGamePadVideo::mockConnected = false;
    if (RemoteGamePad::GetPressedTouch(x, y))
        return 6;
    return 0;
}
