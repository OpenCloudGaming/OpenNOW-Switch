#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define private public
#include "webrtc_session.hpp"
#undef private
#include "webrtc/internal.hpp"
#include "stream_diagnostics.hpp"

#include <cassert>

struct SentMessage {
    uint16_t sid;
    std::vector<uint8_t> bytes;
};

static std::vector<SentMessage> sent_messages;
static int send_result = 0;
static char channel_label[] = "input_channel_v1";
static int open_attempts = 0;
static std::vector<std::string> input_logs;

extern "C" {
int peer_init() { return 0; }
void peer_deinit() {}
PeerConnectionState peer_connection_get_state(PeerConnection*) {
    return PEER_CONNECTION_COMPLETED;
}
const char* peer_connection_state_to_string(PeerConnectionState) { return "completed"; }
char* peer_connection_lookup_sid_label(PeerConnection*, uint16_t) { return channel_label; }
int peer_connection_datachannel_send_binary_sid(PeerConnection*, char* data, size_t len, uint16_t sid) {
    sent_messages.push_back({sid, std::vector<uint8_t>(data, data + len)});
    return send_result < 0 ? send_result : static_cast<int>(len);
}
int peer_connection_create_datachannel_sid(PeerConnection*, DecpChannelType type, uint16_t,
                                           uint32_t, char* label, char*, uint16_t sid) {
    assert(type == DATA_CHANNEL_RELIABLE && sid == 0);
    assert(std::string(label) == "input_channel_v1");
    ++open_attempts;
    return send_result;
}
}

namespace opennow {
bool StreamDiagnosticsEnabled() { return false; }
bool SensitiveInputLoggingSuppressed() { return false; }
}

namespace opennow::webrtc::internal {
void AppendInputLog(const std::string& line) { input_logs.push_back(line); }
void AppendStreamLog(const std::string&) {}
std::string HexPreview(const uint8_t*, size_t, size_t) { return {}; }
std::string PreviewText(const std::string&, size_t) { return {}; }
uint64_t NowUs() { return 0x0102030405060708ULL; }
}

WebRtcSession::WebRtcSession(const std::string&, const std::string&, const std::string&,
                             const std::string&, int, const std::vector<opennow::IceServerInfo>&) {}
WebRtcSession::~WebRtcSession() = default;
SignalingClient::~SignalingClient() = default;
WebSocketClient::~WebSocketClient() = default;
FFmpegVideoDecoder::~FFmpegVideoDecoder() = default;
int FFmpegVideoDecoder::setup(int, int, int, int, void*, int) { return 0; }
void FFmpegVideoDecoder::cleanup() {}
int FFmpegVideoDecoder::submit_decode_unit(uint8_t*, int, int64_t) { return 0; }
int FFmpegVideoDecoder::capabilities() const { return 0; }
VideoDecodeStats* FFmpegVideoDecoder::video_decode_stats() { return nullptr; }
struct AudioPipeline::Impl {};
AudioPipeline::~AudioPipeline() = default;

static void RunScenario(const std::string& scenario) {
    sent_messages.clear();
    send_result = 0;
    open_attempts = 0;
    input_logs.clear();
    channel_label[0] = 'i';
    WebRtcSession session("", "", "", "", 0, {});
    session.pc_ = reinterpret_cast<PeerConnection*>(&session);
    session.on_datachannel_open();
    session.datachannel_open_requested_ = true;
    session.reliable_input_channel_requested_ = true;
    assert(opennow::webrtc::internal::InputEncodingSelfTest());
    if (scenario == "echo") {
        const uint8_t handshake[] = {0x0e, 0x02, 0x03, 0x00};
        session.on_datachannel_message(reinterpret_cast<const char*>(handshake), sizeof(handshake), 0);
        assert(!sent_messages.empty());
        assert(sent_messages[0].sid == 0);
        assert(sent_messages[0].bytes == std::vector<uint8_t>(std::begin(handshake), std::end(handshake)));
        assert(session.input_ready_ && session.input_protocol_version_ == 3);
        assert(session.send_gamepad_input(0, 0x0101, 0x1000, 0, 0, 0, 0, 0, 0));
        assert(sent_messages.back().bytes.size() == 50);
        assert(sent_messages.back().bytes[0] == 0x23);
        assert(sent_messages.back().bytes[9] == 0x21);
        session.send_mouse_left_button(true);
        assert(sent_messages.back().bytes.size() == 28);
        assert(sent_messages.back().bytes[9] == 0x22 && sent_messages.back().bytes[10] == 8);
        session.send_mouse_move(10, -5);
        assert(sent_messages.back().bytes.size() == 34);
        assert(sent_messages.back().bytes[9] == 0x21 && sent_messages.back().bytes[12] == 7);
        session.send_keyboard_key(0x41, 0x1e, 0, true);
        assert(sent_messages.back().bytes.size() == 28);
        assert(sent_messages.back().bytes[9] == 0x22 && sent_messages.back().bytes[10] == 3);
    } else if (scenario == "post-handshake") {
        const uint8_t handshake[] = {0x0e, 0x02, 0x03, 0x00};
        session.on_datachannel_message(reinterpret_cast<const char*>(handshake), sizeof(handshake), 0);
        assert(session.input_ready_ && session.input_protocol_version_ == 3);
        const auto messages_after_handshake = sent_messages.size();
        const uint8_t later_message[] = {0x0e, 0x02, 0x02, 0x00};
        session.on_datachannel_message(reinterpret_cast<const char*>(later_message), sizeof(later_message), 0);
        assert(session.input_ready_ && session.input_protocol_version_ == 3);
        assert(sent_messages.size() == messages_after_handshake);
        const uint8_t legacy_message[] = {0x0e, 0x03, 0x00, 0x00};
        session.on_datachannel_message(reinterpret_cast<const char*>(legacy_message), sizeof(legacy_message), 0);
        assert(session.input_ready_ && session.input_protocol_version_ == 3);
        assert(sent_messages.size() == messages_after_handshake);
        session.on_datachannel_close();
        session.on_datachannel_open();
        session.on_datachannel_message(reinterpret_cast<const char*>(later_message), sizeof(later_message), 0);
        assert(session.input_ready_ && session.input_protocol_version_ == 2);
        assert(sent_messages[messages_after_handshake].bytes ==
               std::vector<uint8_t>(std::begin(later_message), std::end(later_message)));
    } else if (scenario == "retry") {
        const uint8_t handshake[] = {0x0e, 0x02, 0x03, 0x00};
        send_result = -1;
        session.on_datachannel_message(reinterpret_cast<const char*>(handshake), sizeof(handshake), 0);
        assert(!session.input_ready_);
        assert(!session.send_gamepad_input(0, 0x0101, 0, 0, 0, 0, 0, 0, 0));
        session.input_activation_due_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        session.maybe_activate_input();
        assert(!session.input_ready_);
        send_result = 0;
        session.input_activation_due_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        session.maybe_activate_input();
        assert(session.input_ready_ && session.input_protocol_version_ == 3);
        assert(sent_messages.back().bytes == std::vector<uint8_t>(std::begin(handshake), std::end(handshake)));
    } else if (scenario == "recovery") {
        session.input_ready_ = true;
        session.input_protocol_version_ = 3;
        session.input_handshake_acknowledged_ = true;
        session.note_input_send_result(-1, "gamepad");
        session.note_input_send_result(-1, "gamepad");
        assert(session.input_ready_);
        session.note_input_send_result(-1, "gamepad");
        assert(!session.input_ready_);
        session.maybe_activate_input();
        assert(!session.input_ready_);
        session.input_activation_due_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        session.maybe_activate_input();
        assert(session.input_ready_);
        assert(session.input_protocol_version_ == 3);
        assert(input_logs.back().find("HANDSHAKE reactivate protocol=3") == 0);
        assert(session.send_gamepad_input(0, 0x0101, 0, 0, 0, 0, 0, 0, 0));
        assert(sent_messages.back().bytes.size() == 50);
    } else if (scenario == "reset") {
        session.datachannel_open_attempts_ = 60;
        session.reset_input_channel_for_retry("sctp_close");
        assert(session.datachannel_open_attempts_ == 0);
        assert(!session.input_ready_ && !session.datachannel_open_requested_);
        assert(!session.datachannel_opened_);
        assert(session.input_protocol_version_ == 2);
        session.on_datachannel_open();
        session.maybe_open_datachannel();
        assert(open_attempts == 1 && session.datachannel_open_requested_);
    } else if (scenario == "budget") {
        session.datachannel_open_requested_ = false;
        session.reliable_input_channel_requested_ = false;
        send_result = -1;
        for (int attempt = 0; attempt < 65; ++attempt) {
            session.last_datachannel_open_attempt_ = {};
            session.maybe_open_datachannel();
        }
        assert(open_attempts == 60);
        session.on_datachannel_close();
        session.on_datachannel_open();
        send_result = 0;
        session.maybe_open_datachannel();
        assert(open_attempts == 61 && session.datachannel_open_requested_);
    } else if (scenario == "association-wait") {
        session.reset_input_channel_for_retry("sctp_close");
        send_result = -1;
        for (int attempt = 0; attempt < 65; ++attempt) {
            session.last_datachannel_open_attempt_ = {};
            session.maybe_open_datachannel();
        }
        assert(open_attempts == 0 && session.datachannel_open_attempts_ == 0);
        session.on_datachannel_open();
        send_result = 0;
        session.maybe_open_datachannel();
        assert(open_attempts == 1 && session.datachannel_open_requested_);
    } else if (scenario == "bounds") {
        std::vector<uint8_t> oversized(65, 0);
        oversized[0] = 0x0e;
        oversized[1] = 0x02;
        oversized[2] = 0x03;
        session.on_datachannel_message(reinterpret_cast<const char*>(oversized.data()), oversized.size(), 0);
        assert(!session.input_ready_ && session.pending_input_handshake_.empty());
        assert(sent_messages.empty());
        const uint8_t handshake[] = {0x0e, 0x02, 0x03, 0x00};
        session.on_datachannel_message(reinterpret_cast<const char*>(handshake), sizeof(handshake), 2);
        assert(!session.input_ready_ && sent_messages.empty());
        channel_label[0] = 'x';
        session.on_datachannel_message(reinterpret_cast<const char*>(handshake), sizeof(handshake), 0);
        assert(!session.input_ready_ && sent_messages.empty());
    } else if (scenario == "pending-reset") {
        const uint8_t handshake[] = {0x0e, 0x02, 0x03, 0x00};
        send_result = -1;
        session.on_datachannel_message(reinterpret_cast<const char*>(handshake), sizeof(handshake), 0);
        assert(!session.pending_input_handshake_.empty());
        session.on_datachannel_close();
        assert(session.pending_input_handshake_.empty());
        assert(session.input_protocol_version_ == 2 && !session.input_ready_);
        assert(!session.input_handshake_acknowledged_);
    } else if (scenario == "fallback") {
        session.input_activation_due_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        session.maybe_activate_input();
        assert(session.input_ready_ && session.input_protocol_version_ == 2);
        assert(input_logs.back().find("HANDSHAKE fallback protocol=2") == 0);
        assert(session.send_gamepad_input(0, 0x0101, 0, 0, 0, 0, 0, 0, 0));
        assert(sent_messages.back().bytes.size() == 38 && sent_messages.back().bytes[0] == 12);
    } else if (scenario == "legacy") {
        const uint8_t handshake[] = {0x0e, 0x03, 0x00, 0x00};
        session.on_datachannel_message(reinterpret_cast<const char*>(handshake), sizeof(handshake), 0);
        assert(session.input_ready_ && session.input_protocol_version_ == 0x030e);
        assert(sent_messages[0].bytes == std::vector<uint8_t>(std::begin(handshake), std::end(handshake)));
    } else {
        assert(false);
    }
}

int main(int argc, char** argv) {
    if (argc == 2) {
        RunScenario(argv[1]);
        return 0;
    }
    assert(argc == 1);
    for (const auto* scenario : {"echo", "post-handshake", "retry", "recovery", "reset", "budget",
                                 "association-wait", "bounds", "pending-reset", "fallback", "legacy"}) {
        RunScenario(scenario);
        std::printf("PASS webrtc_input_handshake/%s\n", scenario);
    }
}
