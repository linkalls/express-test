#pragma once
// Stub used only for compiling the GamePad bridge without full Cemu/vcpkg.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using uint8 = std::uint8_t;
class Renderer
{
public:
    using ScreenshotSaveFunction =
        std::function<std::optional<std::string>(const std::vector<uint8>&, int, int, bool)>;
    void RequestScreenshot(ScreenshotSaveFunction callback)
    {
        // Ensure the bridge's callback matches Cemu's API signature.
        callback_ = std::move(callback);
    }
private:
    ScreenshotSaveFunction callback_;
};
