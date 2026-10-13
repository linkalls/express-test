// SPDX-License-Identifier: MPL-2.0
// Experimental Cemu-native GamePad bridge.
// This is a WINDOW-FREE framebuffer readback prototype, not a video encoder.
// In particular the stock Vulkan screenshot readback stalls the GPU, so the
// preview is intentionally rate-limited. Replace this with async readback + H264.
//
// The bridge binds ONLY to loopback. adb reverse lets a USB-connected Android
// app reach it without opening an unauthenticated LAN port.
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "Cafe/HW/Latte/Renderer/Renderer.h"

namespace RemoteGamePad
{
    namespace detail
    {
        // Distinct inline variables guarantee one process-wide state even though
        // this header is included by the renderer and input modules.
        inline std::once_flag serverOnce;
        inline std::atomic<bool> connected{false};
        inline std::atomic<bool> capturePending{false};
        inline std::atomic<uint64_t> lastRequestMs{0};
        inline std::atomic<uint32_t> nextSequence{1};
        inline std::atomic<uint16_t> touchX{0};
        inline std::atomic<uint16_t> touchY{0};
        inline std::atomic<bool> touchPressed{false};
        inline std::mutex frameMutex;

        struct Frame
        {
            uint32_t width{0};
            uint32_t height{0};
            uint32_t sequence{0};
            std::vector<uint8_t> rgb;
        };
        inline std::optional<Frame> latestFrame;

        inline uint64_t MonotonicMs()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        inline void WriteU32LE(uint8_t* target, uint32_t value)
        {
            target[0] = static_cast<uint8_t>(value);
            target[1] = static_cast<uint8_t>(value >> 8);
            target[2] = static_cast<uint8_t>(value >> 16);
            target[3] = static_cast<uint8_t>(value >> 24);
        }

        // One peer per Cemu instance. This is deliberately single-user until
        // pairing/authentication can be integrated for Wi-Fi.
        inline bool SendAll(SOCKET socket, const uint8_t* data, size_t bytes)
        {
            while (bytes)
            {
                const int chunk = static_cast<int>(std::min<size_t>(bytes, 65536));
                const int sent = send(socket, reinterpret_cast<const char*>(data), chunk, 0);
                if (sent <= 0)
                    return false;
                data += sent;
                bytes -= static_cast<size_t>(sent);
            }
            return true;
        }

        inline bool SendFrame(SOCKET socket, const Frame& frame)
        {
            uint8_t header[20]{'R', 'G', 'F', '1'};
            WriteU32LE(header + 4, frame.width);
            WriteU32LE(header + 8, frame.height);
            WriteU32LE(header + 12, frame.sequence);
            WriteU32LE(header + 16, static_cast<uint32_t>(frame.rgb.size()));
            return SendAll(socket, header, sizeof(header)) &&
                   SendAll(socket, frame.rgb.data(), frame.rgb.size());
        }

        inline void ProcessTouchPackets(std::vector<uint8_t>& buffer)
        {
            // Fixed 9-byte messages: "TCH1", x(u16 LE), y(u16 LE), down(u8).
            // Normalize the touchscreen coordinates to [0, 65535].
            while (buffer.size() >= 9)
            {
                if (std::memcmp(buffer.data(), "TCH1", 4) != 0)
                {
                    buffer.erase(buffer.begin());
                    continue;
                }
                const uint16_t x = static_cast<uint16_t>(buffer[4] | (buffer[5] << 8));
                const uint16_t y = static_cast<uint16_t>(buffer[6] | (buffer[7] << 8));
                touchX.store(x, std::memory_order_relaxed);
                touchY.store(y, std::memory_order_relaxed);
                touchPressed.store(buffer[8] != 0, std::memory_order_release);
                buffer.erase(buffer.begin(), buffer.begin() + 9);
            }
            // Don't permit unbounded growth from malformed inputs.
            if (buffer.size() > 4096)
                buffer.clear();
        }

        inline void HandleClient(SOCKET client)
        {
            DWORD timeoutMs = 2000;
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

            // Read small control messages while sending the latest RGB frame.
            // The renderer never blocks on this socket or on video encoding.
            std::vector<uint8_t> control;
            control.reserve(1024);
            connected.store(true, std::memory_order_release);
            touchPressed.store(false, std::memory_order_release);
            for (;;)
            {
                fd_set reads;
                FD_ZERO(&reads);
                FD_SET(client, &reads);
                timeval delay{0, 15000};
                const int result = select(0, &reads, nullptr, nullptr, &delay);
                if (result == SOCKET_ERROR)
                    break;
                if (result > 0)
                {
                    char chunk[256];
                    const int count = recv(client, chunk, sizeof(chunk), 0);
                    if (count <= 0)
                        break;
                    control.insert(control.end(), chunk, chunk + count);
                    ProcessTouchPackets(control);
                }

                std::optional<Frame> outgoing;
                {
                    std::lock_guard lock(frameMutex);
                    outgoing = std::move(latestFrame);
                    latestFrame.reset();
                }
                if (outgoing && !SendFrame(client, *outgoing))
                    break;
            }
            // Safety: never leave a GamePad finger pressed after disconnect.
            touchPressed.store(false, std::memory_order_release);
            connected.store(false, std::memory_order_release);
            capturePending.store(false, std::memory_order_release);
            closesocket(client);
        }

        inline void ServerLoop()
        {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                return;

            SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (server == INVALID_SOCKET)
            {
                WSACleanup();
                return;
            }

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(49222);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(server, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
                listen(server, 1) == SOCKET_ERROR)
            {
                closesocket(server);
                WSACleanup();
                return;
            }

            for (;;)
            {
                SOCKET client = accept(server, nullptr, nullptr);
                if (client != INVALID_SOCKET)
                    HandleClient(client);
            }
        }
    } // namespace detail

    inline void Start()
    {
        std::call_once(detail::serverOnce, [] {
            std::thread([] { detail::ServerLoop(); }).detach();
        });
    }

    inline bool Connected()
    {
        return detail::connected.load(std::memory_order_acquire);
    }

    inline bool GetPressedTouch(float& normalizedX, float& normalizedY)
    {
        if (!Connected() || !detail::touchPressed.load(std::memory_order_acquire))
            return false;
        normalizedX = static_cast<float>(detail::touchX.load(std::memory_order_relaxed)) / 65535.0f;
        normalizedY = static_cast<float>(detail::touchY.load(std::memory_order_relaxed)) / 65535.0f;
        return true;
    }

    // Called from Cemu's native DRC source-texture path, never from a
    // window/screen recording API. The callback is invoked asynchronously
    // by Cemu's existing screenshot machinery.
    inline void RequestNativeFrameIfDue(Renderer* renderer)
    {
        if (!Connected() || !renderer)
            return;

        const uint64_t now = detail::MonotonicMs();
        const uint64_t previous = detail::lastRequestMs.load(std::memory_order_relaxed);
        // Stock Vulkan implementation waits for the GPU. Start at <=8 fps.
        constexpr uint64_t kCaptureIntervalMs = 125;
        if (now - previous < kCaptureIntervalMs)
            return;
        bool wasPending = false;
        if (!detail::capturePending.compare_exchange_strong(wasPending, true))
            return;
        detail::lastRequestMs.store(now, std::memory_order_release);
        renderer->RequestScreenshot([](const std::vector<uint8_t>& rgb,
                                      int width, int height, bool mainWindow)
            -> std::optional<std::string>
        {
            detail::capturePending.store(false, std::memory_order_release);
            if (mainWindow || !Connected() || width <= 0 || height <= 0 ||
                width > 4096 || height > 4096 ||
                rgb.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 3)
                return std::nullopt;
            detail::Frame frame;
            frame.width = static_cast<uint32_t>(width);
            frame.height = static_cast<uint32_t>(height);
            frame.sequence = detail::nextSequence.fetch_add(1);
            frame.rgb = rgb;
            std::lock_guard lock(detail::frameMutex);
            detail::latestFrame = std::move(frame);
            return std::nullopt;
        });
    }
} // namespace RemoteGamePad
#endif // _WIN32
