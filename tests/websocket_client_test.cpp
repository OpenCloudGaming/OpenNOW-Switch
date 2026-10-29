#include "websocket_test_transport.hpp"

int main()
{
    using Queue = opennow::websocket::WriteQueue;
    {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& fake = latest();
        std::vector<std::string> messages;
        client.set_on_message([&](const std::string& message) { messages.push_back(message); });
        fake.input = {0x82, 8, '{', '"', 'h', 'b', '"', ':', '1', '}'};
        client.poll();
        client.poll();
        assert(messages == std::vector<std::string>{"{\"hb\":1}"});
        fake.input = {0x02, 3, '{', '"', 'h', 0x89, 1, 'p',
                      0x00, 3, 'b', '"', ':', 0x80, 2, '1', '}'};
        client.poll();
        client.poll();
        assert(messages == std::vector<std::string>({"{\"hb\":1}", "{\"hb\":1}"}));
        assert(frames(fake).size() == 1 && frames(fake)[0].opcode == 0x0a);
        fake.input = {0x02, 10, '{', '"', 't', 'e', 'x', 't', '"', ':', '"', 0xc3,
                      0x80, 3, 0xa9, '"', '}'};
        client.poll();
        client.poll();
        assert(messages.back() == "{\"text\":\"\xc3\xa9\"}");
    }
    {
        const auto dials = transports.size();
        WebSocketClient expired("ws://example.invalid");
        assert(!expired.connect(Queue::Clock::now()));
        assert(expired.failure_kind() == WebSocketClient::FailureKind::Transient);
        assert(transports.size() == dials);
        next_upgrade = UpgradeResponse::SlowDrip;
        WebSocketClient slow("ws://example.invalid");
        const auto started = Queue::Clock::now();
        assert(!slow.connect(started + std::chrono::milliseconds(50)));
        assert(Queue::Clock::now() - started < std::chrono::seconds(1));
        assert(slow.failure_kind() == WebSocketClient::FailureKind::Transient);
        assert(latest().closed);
        assert(latest().connect_timeout_ms > 0 && latest().connect_timeout_ms <= 50);
        assert(latest().timeout_ms > 0 && latest().timeout_ms <= 50);
    }
    for (const CURLcode error : {CURLE_OK, CURLE_RECV_ERROR}) {
        WebSocketClient client("ws://example.invalid");
        assert(client.connect());
        auto& fake = latest();
        std::vector<std::string> messages;
        client.set_on_message([&](const std::string& message) { messages.push_back(message); });
        for (size_t i = 0; i < 32; ++i)
            fake.input.insert(fake.input.end(), {0x81, 1, 'x'});
        fake.input.insert(fake.input.end(), {0x81, 2, 'y'});
        fake.receive_result = error;
        for (size_t i = 0; i < 4; ++i)
            client.poll();
        assert(messages == std::vector<std::string>(32, "x"));
        assert(!client.is_connected());
        assert(fake.closed);
        assert(client.get_last_error() == (error == CURLE_OK
            ? "Remote endpoint closed the signaling connection" : "Receive error: fake transport error"));
        assert(client.connect());
        auto& fresh = latest();
        fresh.input = {0x81, 1, 'z'};
        client.poll();
        client.poll();
        assert(messages.back() == "z");
        assert(client.is_connected());
    }
    {
        next_upgrade = UpgradeResponse::SlowDrip;
        WebSocketClient client("ws://example.invalid");
        const auto started = Queue::Clock::now();
        assert(!client.connect());
        assert(Queue::Clock::now() - started < std::chrono::seconds(6));
        assert(latest().closed);
        assert(client.get_last_error() == "Handshake timeout");
    }
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
        assert(messages == std::vector<std::string>{"bi"});
        client.poll();
        assert(messages == std::vector<std::string>({"bi", std::string(20, 'x')}));
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
