#pragma once

#include "WebSocketClient.hpp"
#include "websocket_handshake.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>
#include <cstdarg>

namespace
{

enum class UpgradeResponse { Valid, Bare, WrongAccept, SlowDrip, Unavailable, Unauthorized };
UpgradeResponse next_upgrade = UpgradeResponse::Valid;
CURLcode next_perform_result = CURLE_OK;

struct FakeTransport
{
    struct Write
    {
        CURLcode result;
        size_t bytes;
    };

    bool handshaking = true;
    bool closed = false;
    size_t calls = 0;
    std::deque<Write> writes;
    std::vector<uint8_t> output;
    std::vector<uint8_t> input;
    CURLcode receive_result = CURLE_AGAIN;
    UpgradeResponse upgrade = std::exchange(next_upgrade, UpgradeResponse::Valid);
    std::string request;
    size_t upgrade_offset = 0;
    CURLcode perform_result = std::exchange(next_perform_result, CURLE_OK);
    long connect_timeout_ms = 0;
    long timeout_ms = 0;
};

std::vector<std::unique_ptr<FakeTransport>> transports;

FakeTransport& transport(CURL* curl)
{
    auto& result = *reinterpret_cast<FakeTransport*>(curl);
    assert(!result.closed);
    return result;
}

FakeTransport& latest()
{
    return *transports.back();
}

struct Frame
{
    uint8_t opcode;
    std::string payload;
};

inline std::vector<Frame> frames(const FakeTransport& fake)
{
    std::vector<Frame> result;
    size_t offset = 0;
    while (offset < fake.output.size()) {
        const auto& bytes = fake.output;
        assert(bytes.size() - offset >= 6);
        const uint8_t opcode = bytes[offset++];
        assert(opcode & 0x80);
        assert(bytes[offset] & 0x80);
        size_t length = bytes[offset++] & 0x7f;
        if (length == 126 || length == 127) {
            const size_t length_bytes = length == 126 ? 2 : 8;
            assert(bytes.size() - offset >= length_bytes + 4);
            length = 0;
            for (size_t i = 0; i < length_bytes; ++i)
                length = (length << 8) | bytes[offset++];
        }
        const size_t mask = offset;
        offset += 4;
        assert(bytes.size() - offset >= length);
        std::string payload;
        for (size_t i = 0; i < length; ++i)
            payload.push_back(bytes[offset++] ^ bytes[mask + i % 4]);
        result.push_back({static_cast<uint8_t>(opcode & 0x0f), std::move(payload)});
    }
    return result;
}

}

extern "C" {

CURL* curl_easy_init()
{
    transports.push_back(std::make_unique<FakeTransport>());
    return reinterpret_cast<CURL*>(transports.back().get());
}

void curl_easy_cleanup(CURL* curl)
{
    transport(curl).closed = true;
}

CURLcode curl_easy_setopt(CURL* curl, CURLoption option, ...)
{
    if (option == CURLOPT_CONNECTTIMEOUT_MS || option == CURLOPT_TIMEOUT_MS) {
        va_list arguments;
        va_start(arguments, option);
        const long value = va_arg(arguments, long);
        va_end(arguments);
        if (option == CURLOPT_CONNECTTIMEOUT_MS)
            transport(curl).connect_timeout_ms = value;
        else
            transport(curl).timeout_ms = value;
    }
    return CURLE_OK;
}

CURLcode curl_easy_perform(CURL* curl)
{
    return transport(curl).perform_result;
}

const char* curl_easy_strerror(CURLcode)
{
    return "fake transport error";
}

CURLcode curl_easy_send(CURL* curl, const void* buffer, size_t length, size_t* sent)
{
    auto& fake = transport(curl);
    if (fake.handshaking) {
        fake.request.append(static_cast<const char*>(buffer), length);
        *sent = length;
        return CURLE_OK;
    }
    ++fake.calls;
    assert(length <= opennow::websocket::WriteQueue::MaximumWriteBytes);
    FakeTransport::Write write{CURLE_OK, length};
    if (!fake.writes.empty()) {
        write = fake.writes.front();
        fake.writes.pop_front();
    }
    *sent = write.result == CURLE_OK ? std::min(length, write.bytes) : 0;
    const auto* bytes = static_cast<const uint8_t*>(buffer);
    fake.output.insert(fake.output.end(), bytes, bytes + *sent);
    return write.result;
}

CURLcode curl_easy_recv(CURL* curl, void* buffer, size_t length, size_t* received)
{
    auto& fake = transport(curl);
    if (fake.handshaking) {
        const std::string key_header = "Sec-WebSocket-Key: ";
        const size_t start = fake.request.find(key_header);
        assert(start != std::string::npos);
        const size_t value = start + key_header.size();
        const auto accept = opennow::websocket::AcceptForKey(
            fake.request.substr(value, fake.request.find("\r\n", value) - value));
        assert(accept);
        const std::string status = fake.upgrade == UpgradeResponse::Unavailable ? "503 Unavailable"
            : fake.upgrade == UpgradeResponse::Unauthorized ? "403 Forbidden" : "101 Switching Protocols";
        const std::string response = "HTTP/1.1 " + status + "\r\n" +
            (fake.upgrade == UpgradeResponse::Bare ? std::string() :
                "Upgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: " +
                (fake.upgrade == UpgradeResponse::WrongAccept ? "synthetic-secret" : *accept) + "\r\n") + "\r\n";
        if (fake.upgrade == UpgradeResponse::SlowDrip && fake.upgrade_offset < 55) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            *received = 1;
        } else {
            *received = response.size() - fake.upgrade_offset;
        }
        assert(length >= *received);
        std::memcpy(buffer, response.data() + fake.upgrade_offset, *received);
        fake.upgrade_offset += *received;
        fake.handshaking = fake.upgrade_offset != response.size();
        return CURLE_OK;
    }
    *received = std::min(length, fake.input.size());
    if (*received == 0)
        return fake.receive_result;
    std::memcpy(buffer, fake.input.data(), *received);
    fake.input.erase(fake.input.begin(), fake.input.begin() + *received);
    return CURLE_OK;
}

}
