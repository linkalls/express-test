// SPDX-License-Identifier: MPL-2.0
// Included into OpenGLRenderer.cpp, after OpenGL/CemuGL declarations.
//
// Asynchronous DRC readback through a bounded PBO+GLsync ring.
// This is not screen/window capture. The GPU can complete the transfer later;
// glClientWaitSync(..., timeout=0) never intentionally blocks the render thread.
#include "remote/RemoteGamePadVideo.h"
#include <array>
#include <chrono>

namespace RemoteGamePadAsyncGL
{
struct Slot
{
    GLuint buffer = 0;
    GLsync fence = nullptr;
    int width = 0;
    int height = 0;
    bool pending = false;
};

inline std::array<Slot, 4>& slots()
{
    static std::array<Slot, 4> result{};
    return result;
}
inline uint64_t clockMicroseconds()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()
    );
}
inline void ResetPending(Slot& slot)
{
    if (slot.fence) {
        glDeleteSync(slot.fence);
        slot.fence = nullptr;
    }
    slot.pending = false;
}
inline void Capture(LatteTextureView* view, bool padView)
{
    if (!padView || !RemoteGamePadVideo::Connected() || !view ||
        !glGetTextureSubImage || !glFenceSync || !glClientWaitSync)
        return;

    // Do not bind/use a framebuffer, OS window, or desktop-capture API.
    auto* texture = static_cast<LatteTextureViewGL*>(view);
    if (texture->glTexTarget != GL_TEXTURE_2D)
        return;

    int width = 0;
    int height = 0;
    view->baseTexture->GetEffectiveSize(width, height, 0);
    if (width < 2 || height < 2 || width > 1920 || height > 1080)
        return;

    auto& ring = slots();
    for (auto& slot : ring)
    {
        if (!slot.pending)
            continue;
        const GLenum done = glClientWaitSync(slot.fence, 0, 0);
        if (done != GL_ALREADY_SIGNALED && done != GL_CONDITION_SATISFIED)
            continue;

        const size_t bytes = static_cast<size_t>(slot.width) * slot.height * 3;
        GLint previous = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previous);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.buffer);
        if (const auto* data = static_cast<const uint8_t*>(
                glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes, GL_MAP_READ_BIT)))
        {
            std::vector<uint8_t> rgb(data, data + bytes);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            // This enqueues only the newest frame. The encoder never blocks GL.
            RemoteGamePadVideo::OfferFrame(std::move(rgb), slot.width, slot.height);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previous));
        ResetPending(slot);
    }

    // A 60Hz producer cannot make a 30fps game render at 60fps.
    // Avoid duplicate capture calls inside one frame and excessive PBO traffic.
    static uint64_t lastCaptureUs = 0;
    const uint64_t nowUs = clockMicroseconds();
    if (nowUs - lastCaptureUs < 16'000)
        return;
    lastCaptureUs = nowUs;

    Slot* available = nullptr;
    for (auto& slot : ring)
    {
        if (!slot.pending) {
            available = &slot;
            break;
        }
    }
    if (!available)
        return; // GPU is late; never force a synchronous wait.

    if (!available->buffer)
        glGenBuffers(1, &available->buffer);
    if (!available->buffer)
        return;

    const size_t bytes = static_cast<size_t>(width) * height * 3;
    GLint previousBuffer = 0;
    GLint previousAlignment = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousAlignment);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, available->buffer);
    glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTextureSubImage(texture->glTexId, 0, 0, 0, 0, width, height, 1,
                         GL_RGB, GL_UNSIGNED_BYTE, static_cast<GLsizei>(bytes), nullptr);
    available->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    available->width = width;
    available->height = height;
    available->pending = available->fence != nullptr;
    glPixelStorei(GL_PACK_ALIGNMENT, previousAlignment);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousBuffer));
}
} // namespace RemoteGamePadAsyncGL
