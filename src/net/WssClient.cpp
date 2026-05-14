#include "opennow/net/WssClient.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

#if defined(OPENNOW_HAS_MBEDTLS_TLS)
#include <arpa/inet.h>
#include <fcntl.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/sha1.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace opennow::net {

namespace {

struct ParsedUrl {
    std::string host;
    std::string port = "443";
    std::string path = "/";
};

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

ParsedUrl parseWssUrl(const std::string& url) {
    if (!startsWith(url, "wss://")) {
        throw std::runtime_error("WSS URL must start with wss://");
    }

    const auto hostStart = 6u;
    const auto pathStart = url.find('/', hostStart);
    auto hostPort = pathStart == std::string::npos ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
    ParsedUrl parsed;
    parsed.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);

    if (!hostPort.empty() && hostPort.front() == '[') {
        const auto end = hostPort.find(']');
        if (end == std::string::npos) {
            throw std::runtime_error("invalid bracketed IPv6 WSS host");
        }
        parsed.host = hostPort.substr(1, end - 1);
        if (end + 1 < hostPort.size() && hostPort[end + 1] == ':') {
            parsed.port = hostPort.substr(end + 2);
        }
        return parsed;
    }

    const auto colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        parsed.host = hostPort.substr(0, colon);
        parsed.port = hostPort.substr(colon + 1);
    } else {
        parsed.host = hostPort;
    }

    if (parsed.host.empty()) {
        throw std::runtime_error("WSS URL has no host");
    }
    return parsed;
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

#if defined(OPENNOW_HAS_MBEDTLS_TLS)

std::string mbedError(int code) {
    std::array<char, 160> buffer{};
    mbedtls_strerror(code, buffer.data(), buffer.size());
    return buffer.data();
}

std::string base64Encode(const unsigned char* data, std::size_t size) {
    std::size_t outSize = 0;
    mbedtls_base64_encode(nullptr, 0, &outSize, data, size);
    std::string out(outSize, '\0');
    const auto rc = mbedtls_base64_encode(
        reinterpret_cast<unsigned char*>(out.data()),
        out.size(),
        &outSize,
        data,
        size);
    if (rc != 0) {
        throw std::runtime_error("base64 encode failed: " + mbedError(rc));
    }
    out.resize(outSize);
    return out;
}

std::string websocketAcceptFor(const std::string& key) {
    static constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const auto input = key + kGuid;
    std::array<unsigned char, 20> digest{};
    mbedtls_sha1(
        reinterpret_cast<const unsigned char*>(input.data()),
        input.size(),
        digest.data());
    return base64Encode(digest.data(), digest.size());
}

int setBlocking(int fd, bool blocking) {
    const auto flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
}

bool waitReadable(int fd, std::chrono::milliseconds timeout) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    return select(fd + 1, &readSet, nullptr, nullptr, &tv) > 0;
}

bool waitWritable(int fd, std::chrono::milliseconds timeout) {
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(fd, &writeSet);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    return select(fd + 1, nullptr, &writeSet, nullptr, &tv) > 0;
}

int sslWriteWithTimeout(mbedtls_ssl_context* ssl, int fd, const unsigned char* data, std::size_t size, std::chrono::milliseconds timeout) {
    std::size_t written = 0;
    while (written < size) {
        const auto rc = mbedtls_ssl_write(ssl, data + written, size - written);
        if (rc > 0) {
            written += static_cast<std::size_t>(rc);
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
            if (!waitReadable(fd, timeout)) {
                return MBEDTLS_ERR_SSL_TIMEOUT;
            }
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (!waitWritable(fd, timeout)) {
                return MBEDTLS_ERR_SSL_TIMEOUT;
            }
            continue;
        }
        return rc;
    }
    return static_cast<int>(written);
}

int sslReadWithTimeout(mbedtls_ssl_context* ssl, int fd, unsigned char* data, std::size_t size, std::chrono::milliseconds timeout) {
    for (;;) {
        const auto rc = mbedtls_ssl_read(ssl, data, size);
        if (rc > 0) {
            return rc;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
            if (!waitReadable(fd, timeout)) {
                return MBEDTLS_ERR_SSL_TIMEOUT;
            }
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (!waitWritable(fd, timeout)) {
                return MBEDTLS_ERR_SSL_TIMEOUT;
            }
            continue;
        }
        return rc;
    }
}

std::array<unsigned char, 4> randomMask() {
    std::array<unsigned char, 4> mask{};
    std::random_device rd;
    for (auto& byte : mask) {
        byte = static_cast<unsigned char>(rd());
    }
    return mask;
}

#endif

} // namespace

struct WssClient::Impl {
#if defined(OPENNOW_HAS_MBEDTLS_TLS)
    int fd = -1;
    mbedtls_ssl_context ssl{};
    mbedtls_ssl_config conf{};
    mbedtls_x509_crt cacert{};
    mbedtls_entropy_context entropy{};
    mbedtls_ctr_drbg_context ctrDrbg{};
    bool connected = false;

    Impl() {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_x509_crt_init(&cacert);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctrDrbg);
    }

    ~Impl() {
        close();
        mbedtls_ctr_drbg_free(&ctrDrbg);
        mbedtls_entropy_free(&entropy);
        mbedtls_x509_crt_free(&cacert);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ssl_free(&ssl);
    }

    void close() {
        if (connected) {
            mbedtls_ssl_close_notify(&ssl);
        }
        connected = false;
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
#endif
};

WssClient::WssClient() : impl_(new Impl()) {}

WssClient::~WssClient() {
    delete impl_;
}

WssResult WssClient::connect(const WssConnectOptions& options) {
#if defined(OPENNOW_HAS_MBEDTLS_TLS)
    try {
        close();
        const auto parsed = parseWssUrl(options.url);

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* results = nullptr;
        const auto gai = getaddrinfo(parsed.host.c_str(), parsed.port.c_str(), &hints, &results);
        if (gai != 0 || !results) {
            return {false, "getaddrinfo failed for " + parsed.host};
        }

        for (auto* ai = results; ai; ai = ai->ai_next) {
            impl_->fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (impl_->fd < 0) {
                continue;
            }
            setBlocking(impl_->fd, false);
            const auto rc = ::connect(impl_->fd, ai->ai_addr, ai->ai_addrlen);
            if (rc == 0 || waitWritable(impl_->fd, options.timeout)) {
                break;
            }
            ::close(impl_->fd);
            impl_->fd = -1;
        }
        freeaddrinfo(results);

        if (impl_->fd < 0) {
            return {false, "TCP connect failed for " + parsed.host + ":" + parsed.port};
        }

        static constexpr const char* kPers = "opennow-wss";
        auto rc = mbedtls_ctr_drbg_seed(
            &impl_->ctrDrbg,
            mbedtls_entropy_func,
            &impl_->entropy,
            reinterpret_cast<const unsigned char*>(kPers),
            std::strlen(kPers));
        if (rc != 0) {
            return {false, "mbedtls_ctr_drbg_seed failed: " + mbedError(rc)};
        }

        rc = mbedtls_ssl_config_defaults(
            &impl_->conf,
            MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc != 0) {
            return {false, "mbedtls_ssl_config_defaults failed: " + mbedError(rc)};
        }

        mbedtls_ssl_conf_authmode(&impl_->conf, options.verifyPeer ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&impl_->conf, mbedtls_ctr_drbg_random, &impl_->ctrDrbg);

        rc = mbedtls_ssl_setup(&impl_->ssl, &impl_->conf);
        if (rc != 0) {
            return {false, "mbedtls_ssl_setup failed: " + mbedError(rc)};
        }
        rc = mbedtls_ssl_set_hostname(&impl_->ssl, parsed.host.c_str());
        if (rc != 0) {
            return {false, "mbedtls_ssl_set_hostname failed: " + mbedError(rc)};
        }
        mbedtls_ssl_set_bio(
            &impl_->ssl,
            &impl_->fd,
            [](void* ctx, const unsigned char* buf, size_t len) -> int {
                const auto fd = *static_cast<int*>(ctx);
                const auto rc = ::send(fd, buf, len, 0);
                return rc < 0 ? MBEDTLS_ERR_SSL_WANT_WRITE : static_cast<int>(rc);
            },
            [](void* ctx, unsigned char* buf, size_t len) -> int {
                const auto fd = *static_cast<int*>(ctx);
                const auto rc = ::recv(fd, buf, len, 0);
                return rc < 0 ? MBEDTLS_ERR_SSL_WANT_READ : static_cast<int>(rc);
            },
            nullptr);

        for (;;) {
            rc = mbedtls_ssl_handshake(&impl_->ssl);
            if (rc == 0) {
                break;
            }
            if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
                if (!waitReadable(impl_->fd, options.timeout)) {
                    return {false, "TLS handshake timed out waiting for read"};
                }
                continue;
            }
            if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (!waitWritable(impl_->fd, options.timeout)) {
                    return {false, "TLS handshake timed out waiting for write"};
                }
                continue;
            }
            return {false, "TLS handshake failed: " + mbedError(rc)};
        }

        std::array<unsigned char, 16> keyBytes{};
        mbedtls_ctr_drbg_random(&impl_->ctrDrbg, keyBytes.data(), keyBytes.size());
        const auto key = base64Encode(keyBytes.data(), keyBytes.size());
        const auto expectedAccept = websocketAcceptFor(key);

        std::ostringstream request;
        request
            << "GET " << parsed.path << " HTTP/1.1\r\n"
            << "Host: " << parsed.host << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Version: 13\r\n"
            << "Sec-WebSocket-Key: " << key << "\r\n"
            << "Origin: " << options.origin << "\r\n"
            << "User-Agent: " << options.userAgent << "\r\n";
        if (!options.protocol.empty()) {
            request << "Sec-WebSocket-Protocol: " << options.protocol << "\r\n";
        }
        request << "\r\n";

        const auto req = request.str();
        rc = sslWriteWithTimeout(&impl_->ssl, impl_->fd, reinterpret_cast<const unsigned char*>(req.data()), req.size(), options.timeout);
        if (rc < 0) {
            return {false, "WebSocket handshake write failed: " + mbedError(rc)};
        }

        std::string response;
        std::array<unsigned char, 1> byte{};
        while (response.find("\r\n\r\n") == std::string::npos) {
            rc = sslReadWithTimeout(&impl_->ssl, impl_->fd, byte.data(), 1, options.timeout);
            if (rc <= 0) {
                return {false, "WebSocket handshake read failed: " + mbedError(rc)};
            }
            response.push_back(static_cast<char>(byte[0]));
            if (response.size() > 8192) {
                return {false, "WebSocket handshake response too large"};
            }
        }

        std::istringstream lines(response);
        std::string statusLine;
        std::getline(lines, statusLine);
        if (statusLine.find(" 101 ") == std::string::npos) {
            return {false, "WebSocket upgrade rejected: " + trim(statusLine)};
        }

        bool acceptOk = false;
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            const auto name = lower(trim(line.substr(0, colon)));
            const auto value = trim(line.substr(colon + 1));
            if (name == "sec-websocket-accept" && value == expectedAccept) {
                acceptOk = true;
            }
        }
        if (!acceptOk) {
            return {false, "WebSocket accept header validation failed"};
        }

        impl_->connected = true;
        return {true, {}};
    } catch (const std::exception& ex) {
        close();
        return {false, ex.what()};
    }
#else
    (void) options;
    return {false, "WssClient was built without mbedTLS TLS support"};
#endif
}

WssResult WssClient::sendText(const std::string& text) {
#if defined(OPENNOW_HAS_MBEDTLS_TLS)
    if (!impl_->connected) {
        return {false, "WebSocket is not connected"};
    }

    std::vector<unsigned char> frame;
    frame.push_back(0x81);
    const auto len = text.size();
    if (len <= 125) {
        frame.push_back(static_cast<unsigned char>(0x80 | len));
    } else if (len <= 0xffff) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<unsigned char>((len >> 8) & 0xff));
        frame.push_back(static_cast<unsigned char>(len & 0xff));
    } else {
        frame.push_back(0x80 | 127);
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<unsigned char>((static_cast<std::uint64_t>(len) >> shift) & 0xff));
        }
    }
    const auto mask = randomMask();
    frame.insert(frame.end(), mask.begin(), mask.end());
    for (std::size_t i = 0; i < text.size(); ++i) {
        frame.push_back(static_cast<unsigned char>(text[i]) ^ mask[i % 4]);
    }

    const auto rc = sslWriteWithTimeout(&impl_->ssl, impl_->fd, frame.data(), frame.size(), std::chrono::milliseconds(10000));
    if (rc < 0) {
        return {false, "WebSocket write failed: " + mbedError(rc)};
    }
    return {true, {}};
#else
    (void) text;
    return {false, "WssClient was built without mbedTLS TLS support"};
#endif
}

WssResult WssClient::receiveText(std::string& text, std::chrono::milliseconds timeout) {
#if defined(OPENNOW_HAS_MBEDTLS_TLS)
    text.clear();
    if (!impl_->connected) {
        return {false, "WebSocket is not connected"};
    }

    std::array<unsigned char, 2> header{};
    auto rc = sslReadWithTimeout(&impl_->ssl, impl_->fd, header.data(), header.size(), timeout);
    if (rc <= 0) {
        return {false, "WebSocket read failed: " + mbedError(rc)};
    }
    while (rc < 2) {
        const auto more = sslReadWithTimeout(&impl_->ssl, impl_->fd, header.data() + rc, 2 - rc, timeout);
        if (more <= 0) {
            return {false, "WebSocket header read failed: " + mbedError(more)};
        }
        rc += more;
    }

    const auto opcode = header[0] & 0x0f;
    const bool masked = (header[1] & 0x80) != 0;
    std::uint64_t len = header[1] & 0x7f;
    if (len == 126) {
        std::array<unsigned char, 2> ext{};
        rc = sslReadWithTimeout(&impl_->ssl, impl_->fd, ext.data(), ext.size(), timeout);
        if (rc != 2) {
            return {false, "WebSocket extended length read failed"};
        }
        len = (static_cast<std::uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        std::array<unsigned char, 8> ext{};
        rc = sslReadWithTimeout(&impl_->ssl, impl_->fd, ext.data(), ext.size(), timeout);
        if (rc != 8) {
            return {false, "WebSocket extended length read failed"};
        }
        len = 0;
        for (auto b : ext) {
            len = (len << 8) | b;
        }
    }

    std::array<unsigned char, 4> mask{};
    if (masked) {
        rc = sslReadWithTimeout(&impl_->ssl, impl_->fd, mask.data(), mask.size(), timeout);
        if (rc != 4) {
            return {false, "WebSocket mask read failed"};
        }
    }

    if (len > 4 * 1024 * 1024) {
        return {false, "WebSocket frame too large"};
    }
    std::vector<unsigned char> payload(static_cast<std::size_t>(len));
    std::size_t offset = 0;
    while (offset < payload.size()) {
        rc = sslReadWithTimeout(&impl_->ssl, impl_->fd, payload.data() + offset, payload.size() - offset, timeout);
        if (rc <= 0) {
            return {false, "WebSocket payload read failed: " + mbedError(rc)};
        }
        offset += static_cast<std::size_t>(rc);
    }
    if (masked) {
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] ^= mask[i % 4];
        }
    }

    if (opcode == 0x8) {
        impl_->connected = false;
        return {false, "WebSocket closed by peer"};
    }
    if (opcode == 0x9) {
        std::vector<unsigned char> pong;
        pong.push_back(0x8a);
        if (payload.size() <= 125) {
            pong.push_back(static_cast<unsigned char>(0x80 | payload.size()));
            const auto pongMask = randomMask();
            pong.insert(pong.end(), pongMask.begin(), pongMask.end());
            for (std::size_t i = 0; i < payload.size(); ++i) {
                pong.push_back(payload[i] ^ pongMask[i % 4]);
            }
            const auto writeRc = sslWriteWithTimeout(&impl_->ssl, impl_->fd, pong.data(), pong.size(), timeout);
            if (writeRc < 0) {
                return {false, "WebSocket pong write failed: " + mbedError(writeRc)};
            }
        }
        return receiveText(text, timeout);
    }
    if (opcode == 0xa) {
        return receiveText(text, timeout);
    }
    if (opcode != 0x1 && opcode != 0x0) {
        return {false, "WebSocket non-text frame received"};
    }

    text.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
    return {true, {}};
#else
    (void) text;
    (void) timeout;
    return {false, "WssClient was built without mbedTLS TLS support"};
#endif
}

void WssClient::close() {
#if defined(OPENNOW_HAS_MBEDTLS_TLS)
    impl_->close();
#endif
}

} // namespace opennow::net
