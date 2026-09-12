#include "WebSocketClient.hpp"
#include "websocket_handshake.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <memory>
#include <thread>

namespace
{

enum class UpgradeResponse { Valid, Bare, WrongAccept };
UpgradeResponse next_upgrade = UpgradeResponse::Valid;

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

std::vector<Frame> frames(const FakeTransport& fake)
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

CURLcode curl_easy_setopt(CURL*, CURLoption, ...)
{
    return CURLE_OK;
}

CURLcode curl_easy_perform(CURL*)
{
    return CURLE_OK;
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
        const std::string response = "HTTP/1.1 101 Switching Protocols\r\n" +
            (fake.upgrade == UpgradeResponse::Bare ? std::string() :
                "Upgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: " +
                (fake.upgrade == UpgradeResponse::WrongAccept ? "synthetic-secret" : *accept) + "\r\n") + "\r\n";
        assert(length >= response.size());
        std::memcpy(buffer, response.data(), response.size());
        *received = response.size();
        fake.handshaking = false;
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

int main()
{
    using Queue = opennow::websocket::WriteQueue;
    for (const auto response : {UpgradeResponse::Bare, UpgradeResponse::WrongAccept}) {
        next_upgrade = response;
        WebSocketClient client("wss://example.invalid");
        assert(!client.connect());
        assert(latest().closed);
        assert(client.get_last_error() == "Invalid WebSocket upgrade response");
    }
    {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& fake = latest();
        std::vector<std::string> messages;
        client.set_on_message([&](const std::string& message) { messages.push_back(message); });
        fake.input = {0x01, 3, '{', '"', 'h', 0x89, 1, 'p'};
        client.poll();
        client.poll();
        assert(messages.empty());
        assert(frames(fake).size() == 1 && frames(fake)[0].opcode == 0x0a);
        fake.input = {0x00, 3, 'b', '"', ':', 0x80, 2, '1', '}'};
        client.poll();
        client.poll();
        assert(messages == std::vector<std::string>{"{\"hb\":1}"});
    }

    for (const std::vector<uint8_t>& input : std::vector<std::vector<uint8_t>>{
             {0x80, 0}, {0x01, 0, 0x81, 0}, {0x09, 0}, {0x89, 126},
             {0x81, 0x80}, {0xc1, 0}, {0x83, 0}}) {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& fake = latest();
        fake.input = input;
        client.poll();
        client.poll();
        assert(!client.is_connected());
        assert(fake.closed);
        assert(client.get_last_error() == "Invalid incoming WebSocket frame");
    }

    {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& fake = latest();
        std::vector<std::string> messages;
        client.set_on_message([&](const std::string& message) { messages.push_back(message); });
        fake.input = {0x02, 1, 'b', 0x80, 1, 'i', 0x01, 0};
        for (size_t i = 0; i < 20; ++i)
            fake.input.insert(fake.input.end(), {0x00, 1, 'x'});
        fake.input.insert(fake.input.end(), {0x80, 0});
        client.poll();
        client.poll();
        assert(messages.empty());
        client.poll();
        assert(messages == std::vector<std::string>{std::string(20, 'x')});
    }

    {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        std::vector<std::string> messages;
        client.set_on_message([&](const std::string& message) { messages.push_back(message); });
        auto& old = latest();
        old.input = {0x01, 1, 'x'};
        client.poll();
        client.poll();
        assert(messages.empty());
        assert(client.connect());
        auto& fresh = latest();
        fresh.input = {0x81, 1, 'y', 0x88, 2, 0x03, 0xe8};
        fresh.receive_result = CURLE_OK;
        client.poll();
        client.poll();
        assert(messages == std::vector<std::string>{"y"});
        assert(client.get_last_error() == "Signaling closed code=1000");
        assert(fresh.closed);
    }

    {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& fake = latest();
        fake.input = {0x01, 127, 0, 0, 0, 0, 0, 0x40, 0, 0};
        fake.input.resize(fake.input.size() + 4 * 1024 * 1024, 'x');
        for (size_t i = 0; i < 66; ++i)
            client.poll();
        assert(client.is_connected());
        fake.input = {0x80, 1, 'x'};
        client.poll();
        client.poll();
        assert(!client.is_connected());
        assert(fake.closed);
        assert(client.get_last_error() == "Incoming WebSocket frame is too large");
    }

    {
        WebSocketClient client("wss://example.invalid/signaling");
        assert(client.connect());
        auto& fake = latest();
        std::vector<std::string> messages{"first", "second", "", std::string(125, 'a'),
                                          std::string(126, 'b'), std::string(65535, 'c'),
                                          std::string(70000, 'd')};
        for (const auto& message : messages)
            client.send_message(message);
        assert(fake.calls == 0);
        for (size_t i = 0; i < 4; ++i) {
            const auto calls = fake.calls;
            client.poll();
            assert(fake.calls - calls <= Queue::MaximumWritesPerPoll);
        }
        const auto sent = frames(fake);
        assert(sent.size() == messages.size());
        for (size_t i = 0; i < messages.size(); ++i) {
            assert(sent[i].opcode == 1);
            assert(sent[i].payload == messages[i]);
        }
    }

    {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& fake = latest();
        client.send_message("first");
        client.send_message("second");
        fake.writes = {{CURLE_OK, 3}, {CURLE_AGAIN, 0}};
        fake.input = {0x89, 2, 'h', 'i'};
        client.poll();
        assert(fake.calls == 2);
        assert(fake.output.size() == 3);
        fake.writes = {{CURLE_AGAIN, 0}};
        client.poll();
        assert(fake.calls == 3);
        assert(fake.output.size() == 3);
        client.poll();
        const auto sent = frames(fake);
        assert(sent.size() == 3);
        assert(sent[0].payload == "first");
        assert(sent[1].payload == "second");
        assert(sent[2].opcode == 0x0a && sent[2].payload == "hi");
    }

    for (const bool remote_close : {false, true}) {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& fake = latest();
        size_t messages = 0;
        client.set_on_message([&](const std::string&) { ++messages; });
        client.send_message("pending");
        fake.writes = {{CURLE_OK, 3}, {CURLE_AGAIN, 0}};
        if (remote_close)
            fake.input = {0x88, 2, 0x03, 0xe8, 0x81, 1, 'x'};
        client.poll();
        fake.writes = {{CURLE_AGAIN, 0}};
        if (remote_close)
            client.poll();
        else
            client.disconnect();
        assert(!client.is_connected());
        assert(!fake.closed);
        assert(messages == 0);
        assert(fake.calls == 3);
        client.send_message("ignored");
        client.poll();
        assert(fake.closed);
        const auto sent = frames(fake);
        assert(sent.size() == 2);
        assert(sent[0].opcode == 1 && sent[0].payload == "pending");
        assert(sent[1].opcode == 8 && sent[1].payload == std::string("\x03\xe8", 2));
        if (remote_close)
            assert(client.get_last_error() == "Signaling closed code=1000");
    }

    {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& fake = latest();
        client.set_on_message([&](const std::string& message) { client.send_message(message); });
        fake.input = {0x81, 1, 'x'};
        client.poll();
        client.poll();
        auto sent = frames(fake);
        assert(sent.size() == 1 && sent[0].payload == "x");
        fake.input = {0x88, 2, 0x03, 0xe8};
        client.poll();
        client.poll();
        assert(!client.is_connected());
        assert(fake.closed);
        sent = frames(fake);
        assert(sent.size() == 2 && sent[1].opcode == 8);
    }

    for (const bool byte_overflow : {false, true}) {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& old = latest();
        if (byte_overflow) {
            client.send_message(std::string(4 * 1024 * 1024, 'a'));
            old.writes = {{CURLE_OK, 3}, {CURLE_AGAIN, 0}};
            client.poll();
        } else {
            for (size_t i = 0; i < Queue::MaximumFrames; ++i)
                client.send_message("pending");
        }
        assert(client.is_connected());
        client.send_message("overflow");
        assert(!client.is_connected());
        assert(old.closed);
        assert(client.get_last_error() == "Outgoing WebSocket queue is full");
        const auto old_calls = old.calls;
        client.poll();
        assert(old.calls == old_calls);
        assert(client.connect());
        auto& fresh = latest();
        assert(&fresh != &old);
        assert(client.get_last_error().empty());
        client.send_message("fresh");
        client.poll();
        const auto sent = frames(fresh);
        assert(sent.size() == 1 && sent[0].payload == "fresh");
    }

    for (const CURLcode error : {CURLE_OK, CURLE_SEND_ERROR}) {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& fake = latest();
        client.send_message("pending");
        fake.writes = {{error, 0}};
        client.poll();
        assert(!client.is_connected());
        assert(fake.closed);
        assert(fake.calls == 1);
        assert(client.get_last_error() == (error == CURLE_OK ? "Send stalled" :
                                          "Send error: fake transport error"));
        client.poll();
        client.disconnect();
        assert(fake.calls == 1);
    }

    {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& fake = latest();
        client.send_message(std::string(4 * 1024 * 1024 + 1, 'x'));
        assert(!client.is_connected());
        assert(fake.closed);
        assert(fake.calls == 0);
    }

    for (const CURLcode error : {CURLE_OK, CURLE_RECV_ERROR}) {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& fake = latest();
        client.send_message("pending");
        fake.writes = {{CURLE_OK, 3}, {CURLE_AGAIN, 0}};
        fake.receive_result = error;
        client.poll();
        assert(!client.is_connected());
        assert(fake.closed);
        client.poll();
        assert(fake.calls == 2);
    }

    {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& fake = latest();
        client.send_message("pending");
        fake.writes = {{CURLE_OK, 3}, {CURLE_AGAIN, 0}};
        client.poll();
        std::this_thread::sleep_for(Queue::SendTimeout);
        client.poll();
        assert(!client.is_connected());
        assert(fake.closed);
        assert(fake.calls == 2);
        assert(client.get_last_error() == "Send timeout");
    }

    {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& old = latest();
        client.send_message("pending");
        old.writes = {{CURLE_OK, 3}, {CURLE_AGAIN, 0}, {CURLE_AGAIN, 0}};
        client.disconnect();
        assert(client.connect());
        assert(old.closed);
        auto& fresh = latest();
        client.send_message("fresh");
        client.poll();
        const auto sent = frames(fresh);
        assert(sent.size() == 1 && sent[0].payload == "fresh");
    }

    for (const auto& fake : transports)
        assert(fake->closed);
}
