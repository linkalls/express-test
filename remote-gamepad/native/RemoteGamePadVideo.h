// SPDX-License-Identifier: MPL-2.0
// Optional Wi-Fi video backend for the existing Cemu Camera+Tank build.
//
// * Video source: Cemu's native DRC texture (never desktop capture).
// * Encoder: external FFmpeg using explicitly chosen hardware H264 backend.
// * Network: TLS 1.3 with manually-pinned SHA256 certificate fingerprint.
// * Disabled by default: CEMU_REMOTE_WIFI=1 enables TCP/49224.
// * No implicit software fallback when GPU H264 is unavailable.
//
// WARNING: This is an experimental implementation and must be validated
// under a real Vulkan/OpenGL renderer and Android device.
#pragma once
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <openssl/ssl.h>
#include <openssl/rsa.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace RemoteGamePadVideo
{
namespace detail
{
struct Frame
{
    std::vector<uint8_t> rgb;
    int width{};
    int height{};
};
inline std::mutex mutex;
inline std::condition_variable cv;
inline std::optional<Frame> latest;
inline std::once_flag started;
inline std::atomic<bool> clientConnected{false};
inline std::function<void(const uint8_t*, size_t)> touchConsumer;

inline void u32le(uint8_t* p, uint32_t v)
{
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
}
inline std::string env(const char* key, const char* fallback = "")
{
    char buffer[4096]{};
    DWORD length = GetEnvironmentVariableA(key, buffer, sizeof(buffer));
    return (length > 0 && length < sizeof(buffer)) ? std::string(buffer, length) : fallback;
}
inline void report(const std::string& s)
{
    OutputDebugStringA(("[Cemu Remote GamePad] " + s + "\n").c_str());
}
inline std::filesystem::path configDir()
{
    auto local = env("LOCALAPPDATA");
    std::filesystem::path root = local.empty() ? std::filesystem::temp_directory_path() : local;
    auto path = root / "CemuRemoteGamePad";
    std::filesystem::create_directories(path);
    return path;
}
inline bool createCertificate(const std::filesystem::path& file)
{
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    bool ok = false;
    if (!context) return false;
    do {
        if (EVP_PKEY_keygen_init(context) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(context, 2048) <= 0 ||
            EVP_PKEY_keygen(context, &key) <= 0) break;
        cert = X509_new();
        if (!cert) break;
        if (X509_set_version(cert, 2) != 1) break;
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert), 365L * 24 * 3600);
        if (X509_set_pubkey(cert, key) != 1) break;
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("Cemu Remote GamePad"), -1, -1, 0);
        X509_set_issuer_name(cert, name);
        if (X509_sign(cert, key, EVP_sha256()) <= 0) break;

        FILE* fp = nullptr;
        if (fopen_s(&fp, file.string().c_str(), "wb") != 0 || !fp) break;
        const bool wrote = PEM_write_PrivateKey(fp, key, nullptr, nullptr, 0, nullptr, nullptr) == 1 &&
                           PEM_write_X509(fp, cert) == 1;
        fclose(fp);
        ok = wrote;
    } while (false);
    X509_free(cert);
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(context);
    return ok;
}

inline std::string loadOrCreateAccessToken()
{
    const auto file = configDir() / "wifi-access-token.txt";
    std::ifstream in(file);
    std::string token;
    if (in >> token) {
        if (token.size() == 32 && token.find_first_not_of("0123456789ABCDEFabcdef") == std::string::npos)
            return token;
        report("Invalid Wi-Fi access token file; refusing to start.");
        return {};
    }
    unsigned char secret[16]{};
    if (RAND_bytes(secret, sizeof(secret)) != 1)
        return {};
    constexpr char digits[] = "0123456789ABCDEF";
    for (unsigned char c : secret) {
        token += digits[c >> 4];
        token += digits[c & 15];
    }
    std::ofstream output(file, std::ios::trunc);
    output << token << "\n";
    if (!output.good()) return {};
    report("Wi-Fi access token generated; copy token from " + file.string());
    return token;
}

inline bool sslReadExact(SSL* connection, unsigned char* bytes, size_t length)
{
    while (length) {
        const int n = SSL_read(connection, bytes, static_cast<int>(length));
        if (n <= 0) return false;
        bytes += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

inline std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> sslContext()
{
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!ctx) return nullptr;
    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION);
    auto path = configDir() / "wifi-identity.pem";
    if (!std::filesystem::exists(path) && !createCertificate(path)) return nullptr;
    const auto file = path.string();
    if (SSL_CTX_use_certificate_file(ctx.get(), file.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx.get(), file.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx.get()) != 1) return nullptr;

    X509* cert = SSL_CTX_get0_certificate(ctx.get());
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int len = 0;
    if (X509_digest(cert, EVP_sha256(), digest, &len) != 1 || len != 32) return nullptr;
    const char* chars = "0123456789ABCDEF";
    std::string fingerprint;
    for (unsigned i = 0; i < len; ++i) {
        fingerprint += chars[digest[i] >> 4];
        fingerprint += chars[digest[i] & 15];
    }
    std::ofstream(configDir() / "wifi-fingerprint.txt", std::ios::trunc) << fingerprint << "\n";
    report("Wi-Fi TLS certificate fingerprint: " + fingerprint);
    return ctx;
}

inline bool sslWriteAll(SSL* ssl, const uint8_t* p, size_t size)
{
    while (size) {
        const int part = static_cast<int>(std::min<size_t>(size, 16384));
        const int sent = SSL_write(ssl, p, part);
        if (sent <= 0) return false;
        p += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

class EncoderPipe
{
public:
    ~EncoderPipe() { stop(); }
    bool start(int width, int height)
    {
        SECURITY_ATTRIBUTES attrs{sizeof(attrs), nullptr, TRUE};
        HANDLE inputRead = nullptr, outputWrite = nullptr;
        if (!CreatePipe(&inputRead, &stdinWrite, &attrs, 1 << 20)) return false;
        SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
        if (!CreatePipe(&stdoutRead, &outputWrite, &attrs, 1 << 20)) {
            CloseHandle(inputRead);
            stop();
            return false;
        }
        SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

        std::string codec = env("CEMU_REMOTE_H264_ENCODER", "h264_amf");
        // An explicit selection avoids untrusted command-line fragments.
        if (codec != "h264_amf" && codec != "h264_nvenc" &&
            codec != "h264_qsv" && codec != "libx264") codec = "h264_amf";
        std::string options;
        if (codec == "h264_amf") options = "-usage ultralowlatency -quality speed";
        else if (codec == "h264_nvenc") options = "-preset p1 -tune ull";
        else if (codec == "h264_qsv") options = "-preset veryfast";
        else options = "-preset ultrafast -tune zerolatency";

        const std::string ffmpeg = env("CEMU_REMOTE_FFMPEG", "ffmpeg.exe");
        // Program is launched directly (no cmd.exe); quote executable path.
        std::string command = "\"" + ffmpeg + "\" -hide_banner -loglevel error -nostdin "
          "-f rawvideo -pixel_format rgb24 -video_size " +
          std::to_string(width) + "x" + std::to_string(height) +
          " -framerate 60 -i pipe:0 -an -vf format=nv12 -c:v " + codec + " " + options +
          " -bf 0 -g 60 -bsf:v h264_metadata=aud=insert -f h264 pipe:1";

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inputRead;
        si.hStdOutput = outputWrite;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        const BOOL created = CreateProcessA(nullptr, command.data(), nullptr, nullptr,
                                            TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(inputRead);
        CloseHandle(outputWrite);
        if (!created) {
            report("FFmpeg launch failed; set CEMU_REMOTE_FFMPEG to ffmpeg.exe");
            stop();
            return false;
        }
        process = pi.hProcess;
        CloseHandle(pi.hThread);
        report("H264 encoder started: " + codec);
        return true;
    }
    bool writeRGB(const std::vector<uint8_t>& rgb)
    {
        if (!stdinWrite) return false;
        size_t offset = 0;
        while (offset < rgb.size()) {
            DWORD written = 0;
            const DWORD n = static_cast<DWORD>(std::min<size_t>(rgb.size() - offset, 65536));
            if (!WriteFile(stdinWrite, rgb.data() + offset, n, &written, nullptr) || !written)
                return false;
            offset += written;
        }
        return true;
    }
    HANDLE encodedReadHandle() const { return stdoutRead; }
    void stop()
    {
        if (stdinWrite) { CloseHandle(stdinWrite); stdinWrite = nullptr; }
        if (process) { TerminateProcess(process, 0); WaitForSingleObject(process, 3000); CloseHandle(process); process = nullptr; }
        if (stdoutRead) { CloseHandle(stdoutRead); stdoutRead = nullptr; }
    }
private:
    HANDLE stdinWrite{};
    HANDLE stdoutRead{};
    HANDLE process{};
};

inline bool isAud(const std::vector<uint8_t>& data, size_t pos)
{
    return pos + 4 <= data.size() && data[pos] == 0 &&
        data[pos + 1] == 0 &&
        ((data[pos + 2] == 1 && (data[pos + 3] & 31) == 9) ||
         (pos + 5 <= data.size() && data[pos + 2] == 0 &&
          data[pos + 3] == 1 && (data[pos + 4] & 31) == 9));
}
inline std::optional<size_t> nextAud(const std::vector<uint8_t>& data, size_t after)
{
    for (size_t i = after; i + 4 < data.size(); ++i)
        if (isAud(data, i)) return i;
    return std::nullopt;
}

inline void clientSession(SSL_CTX* context, SOCKET socket)
{
    SSL* ssl = SSL_new(context);
    if (!ssl) { closesocket(socket); return; }
    SSL_set_fd(ssl, static_cast<int>(socket));
    DWORD timeout = 1000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    if (SSL_accept(ssl) != 1) {
        SSL_free(ssl);
        closesocket(socket);
        return;
    }

    // Mutual application authentication: TLS protects the channel while the
    // PC requires a high-entropy secret token before releasing gameplay video
    // or processing any remote inputs. The public certificate pin is NOT a
    // substitute for this private token.
    const std::string accessToken = loadOrCreateAccessToken();
    std::array<unsigned char, 36> auth{};
    const bool authorized = accessToken.size() == 32 &&
        sslReadExact(ssl, auth.data(), auth.size()) &&
        std::memcmp(auth.data(), "AUTH", 4) == 0 &&
        CRYPTO_memcmp(auth.data() + 4, accessToken.data(), 32) == 0;
    if (!authorized) {
        report("Rejected unauthenticated Wi-Fi client");
        SSL_shutdown(ssl);
        SSL_free(ssl);
        closesocket(socket);
        return;
    }

    std::mutex sslLock;
    std::atomic<bool> running{true};
    EncoderPipe encoder;
    std::thread inputWorker;
    std::thread outputWorker;
    std::vector<uint8_t> control;
    clientConnected.store(true, std::memory_order_release);
    report("Authenticated transport established; waiting for Android fingerprint validation");

    auto cleanup = [&] {
        running.store(false);
        clientConnected.store(false);
        cv.notify_all();
        // Close FFmpeg pipes before joining workers. WriteFile/ReadFile may
        // otherwise remain blocked if the encoder or network stops responding.
        encoder.stop();
        if (inputWorker.joinable()) inputWorker.join();
        if (outputWorker.joinable()) outputWorker.join();
        // Release any pressed touch when the Wi-Fi session disappears.
        if (touchConsumer) {
            const uint8_t release[9]{'T','C','H','1',0,0,0,0,0};
            touchConsumer(release, sizeof(release));
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
        closesocket(socket);
    };

    // First frame determines source resolution.
    Frame first;
    {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(5), [] { return latest.has_value(); });
        if (!latest) { cleanup(); return; }
        first = std::move(*latest);
        latest.reset();
    }
    if (!encoder.start(first.width, first.height)) { cleanup(); return; }

    inputWorker = std::thread([&, frame = std::move(first)]() mutable {
        Frame current = std::move(frame);
        while (running) {
            if (current.width != first.width || current.height != first.height ||
                !encoder.writeRGB(current.rgb)) { running = false; break; }
            std::unique_lock lock(mutex);
            cv.wait_for(lock, std::chrono::milliseconds(30), [&] { return !running || latest.has_value(); });
            if (!running) break;
            if (!latest) continue;
            current = std::move(*latest);
            latest.reset();
        }
    });

    outputWorker = std::thread([&] {
        std::vector<uint8_t> bitstream;
        bitstream.reserve(262144);
        std::array<uint8_t, 16384> temp{};
        uint32_t sequence = 0;
        while (running) {
            DWORD count = 0;
            if (!ReadFile(encoder.encodedReadHandle(), temp.data(), static_cast<DWORD>(temp.size()),
                          &count, nullptr) || count == 0) break;
            bitstream.insert(bitstream.end(), temp.begin(), temp.begin() + count);
            auto firstAud = nextAud(bitstream, 0);
            while (firstAud) {
                const auto next = nextAud(bitstream, *firstAud + 4);
                if (!next) break;
                // Send all parameter sets preceding AUD with its first AU.
                const size_t n = *next;
                if (n > 0 && n < 2 * 1024 * 1024) {
                    uint8_t header[20]{'A', 'V', 'C', '1'};
                    u32le(header + 4, static_cast<uint32_t>(first.width));
                    u32le(header + 8, static_cast<uint32_t>(first.height));
                    u32le(header + 12, ++sequence);
                    u32le(header + 16, static_cast<uint32_t>(n));
                    std::lock_guard lock(sslLock);
                    if (!sslWriteAll(ssl, header, sizeof(header)) ||
                        !sslWriteAll(ssl, bitstream.data(), n)) {
                        running = false;
                        break;
                    }
                }
                bitstream.erase(bitstream.begin(), bitstream.begin() + n);
                firstAud = nextAud(bitstream, 0);
            }
            if (bitstream.size() > 2 * 1024 * 1024) {
                bitstream.clear();
                report("H264 access-unit buffer overflow; dropping until next IDR");
            }
        }
        running = false;
    });

    while (running) {
        fd_set fd{};
        FD_ZERO(&fd);
        FD_SET(socket, &fd);
        timeval tv{0, 25000};
        const int ready = select(0, &fd, nullptr, nullptr, &tv);
        if (ready == SOCKET_ERROR) break;
        if (ready > 0 || SSL_pending(ssl) > 0) {
            uint8_t buf[256]{};
            int n = 0;
            {
                std::lock_guard lock(sslLock);
                n = SSL_read(ssl, buf, sizeof(buf));
            }
            if (n <= 0) break;
            control.insert(control.end(), buf, buf + n);
            if (touchConsumer) touchConsumer(control.data(), control.size());
            // Touch protocol is fixed-size and starts with "TCH1".
            // Keep trailing incomplete messages for the next read.
            size_t complete = control.size() / 9 * 9;
            if (complete) control.erase(control.begin(), control.begin() + complete);
            if (control.size() > 1024) control.clear();
        }
    }
    cleanup();
}

inline void server()
{
    WSADATA ws{};
    if (WSAStartup(MAKEWORD(2,2), &ws) != 0) return;
    auto context = sslContext();
    if (!context) { report("Wi-Fi TLS initialization failed"); WSACleanup(); return; }
    // Generate the access token before accepting clients. The user needs the
    // token file for pairing even when no smartphone has connected yet.
    if (loadOrCreateAccessToken().size() != 32) {
        report("Wi-Fi access-token initialization failed");
        WSACleanup();
        return;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { WSACleanup(); return; }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(49224);
    // USB H264 uses loopback only; Wi-Fi explicitly allows trusted LAN.
    const bool wifiEnabled = env("CEMU_REMOTE_WIFI") == "1";
    address.sin_addr.s_addr = htonl(wifiEnabled ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR) {
        report("Wi-Fi port 49224 unavailable; check Windows Firewall");
        closesocket(listener);
        WSACleanup();
        return;
    }
    report(std::string("TLS 1.3 H264 listening on ") +
           (wifiEnabled ? "private LAN" : "USB loopback") + " port 49224");
    for (;;) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client != INVALID_SOCKET) clientSession(context.get(), client);
    }
}
} // namespace detail

inline void Start(std::function<void(const uint8_t*, size_t)> onTouch)
{
    if (detail::env("CEMU_REMOTE_WIFI") != "1" &&
        detail::env("CEMU_REMOTE_H264_USB") != "1") return;
    std::call_once(detail::started, [callback = std::move(onTouch)]() mutable {
        detail::touchConsumer = std::move(callback);
        std::thread(detail::server).detach();
    });
}
inline bool Connected()
{
    return detail::clientConnected.load(std::memory_order_acquire);
}
inline void OfferFrame(std::vector<uint8_t> rgb, int width, int height)
{
    if (!Connected()) return;
    if (width <= 0 || height <= 0 || width > 1920 || height > 1080 ||
        rgb.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 3) return;
    std::lock_guard lock(detail::mutex);
    detail::latest = detail::Frame{std::move(rgb), width, height};
    detail::cv.notify_one();
}
} // namespace RemoteGamePadVideo
#endif
