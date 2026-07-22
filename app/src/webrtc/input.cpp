#include "webrtc_session.hpp"
#include "stream_diagnostics.hpp"
#include "internal.hpp"

#include <jansson.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace opennow::webrtc::internal;


namespace
{

void PutU16Le(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void PutI16Le(std::vector<uint8_t>& out, int16_t value)
{
    PutU16Le(out, static_cast<uint16_t>(value));
}

void PutU32Le(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

void PutU64Le(std::vector<uint8_t>& out, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
}

void PutU16Be(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void PutU32Be(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void PutU64Be(std::vector<uint8_t>& out, uint64_t value)
{
    for (int i = 7; i >= 0; --i)
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
}

std::vector<uint8_t> WrapSingleInput(int protocol_version, uint64_t timestamp_us,
                                     const std::vector<uint8_t>& payload)
{
    if (protocol_version <= 2)
        return payload;

    std::vector<uint8_t> wrapped;
    wrapped.reserve(10 + payload.size());
    wrapped.push_back(0x23);
    PutU64Be(wrapped, timestamp_us);
    wrapped.push_back(0x22);
    wrapped.insert(wrapped.end(), payload.begin(), payload.end());
    return wrapped;
}

std::vector<uint8_t> WrapReliableGamepad(int protocol_version, uint64_t timestamp_us,
                                         const std::vector<uint8_t>& payload)
{
    if (protocol_version <= 2)
        return payload;

    std::vector<uint8_t> wrapped;
    wrapped.reserve(12 + payload.size());
    wrapped.push_back(0x23);
    PutU64Be(wrapped, timestamp_us);
    wrapped.push_back(0x21);
    PutU16Be(wrapped, static_cast<uint16_t>(payload.size()));
    wrapped.insert(wrapped.end(), payload.begin(), payload.end());
    return wrapped;
}

std::vector<uint8_t> WrapPartiallyReliableGamepad(int protocol_version, uint64_t timestamp_us,
                                                  uint8_t controller_id, uint16_t sequence,
                                                  const std::vector<uint8_t>& payload)
{
    if (protocol_version <= 2)
        return payload;

    std::vector<uint8_t> wrapped;
    wrapped.reserve(16 + payload.size());
    wrapped.push_back(0x23);
    PutU64Be(wrapped, timestamp_us);
    wrapped.push_back(0x26);
    wrapped.push_back(controller_id);
    PutU16Be(wrapped, sequence);
    wrapped.push_back(0x21);
    PutU16Be(wrapped, static_cast<uint16_t>(payload.size()));
    wrapped.insert(wrapped.end(), payload.begin(), payload.end());
    return wrapped;
}

int16_t AxisToI16(float value)
{
    value = std::max(-1.0f, std::min(1.0f, value));
    return static_cast<int16_t>(value * 32767.0f);
}

std::vector<uint8_t> BuildGamepadPayload(uint64_t timestamp_us, uint16_t buttons,
                                         uint8_t left_trigger, uint8_t right_trigger,
                                         int16_t lx, int16_t ly, int16_t rx, int16_t ry)
{
    std::vector<uint8_t> payload;
    payload.reserve(38);
    PutU32Le(payload, 12);
    PutU16Le(payload, 26);
    PutU16Le(payload, 0);
    PutU16Le(payload, 0x0101);
    PutU16Le(payload, 20);
    PutU16Le(payload, buttons);
    PutU16Le(payload, static_cast<uint16_t>(left_trigger | (right_trigger << 8)));
    PutI16Le(payload, lx);
    PutI16Le(payload, ly);
    PutI16Le(payload, rx);
    PutI16Le(payload, ry);
    PutU16Le(payload, 0);
    PutU16Le(payload, 85);
    PutU16Le(payload, 0);
    PutU64Le(payload, timestamp_us);
    return payload;
}

std::vector<uint8_t> BuildMouseButtonPayload(uint64_t timestamp_us, bool pressed)
{
    std::vector<uint8_t> payload;
    payload.reserve(18);
    PutU32Le(payload, pressed ? 8u : 9u);
    payload.push_back(1);
    payload.push_back(0);
    PutU32Be(payload, 0);
    PutU64Be(payload, timestamp_us);
    return payload;
}

std::vector<uint8_t> BuildMouseMovePayload(uint64_t timestamp_us, int16_t dx, int16_t dy)
{
    std::vector<uint8_t> payload;
    payload.reserve(22);
    PutU32Le(payload, 7);
    PutU16Be(payload, static_cast<uint16_t>(dx));
    PutU16Be(payload, static_cast<uint16_t>(dy));
    PutU16Be(payload, 0);
    PutU32Be(payload, 0);
    PutU64Be(payload, timestamp_us);
    return payload;
}

std::vector<uint8_t> BuildKeyboardPayload(uint64_t timestamp_us, uint16_t keycode,
                                          uint16_t scancode, uint16_t modifiers,
                                          bool pressed)
{
    std::vector<uint8_t> payload;
    payload.reserve(18);
    PutU32Le(payload, pressed ? 3u : 4u);
    PutU16Be(payload, keycode);
    PutU16Be(payload, modifiers);
    PutU16Be(payload, scancode);
    PutU64Be(payload, timestamp_us);
    return payload;
}

} // namespace

namespace opennow::webrtc::internal
{

bool InputEncodingSelfTest()
{
    constexpr uint64_t timestamp = 0x0102030405060708ULL;
    const auto raw = BuildGamepadPayload(timestamp, 0x1234, 0x56, 0x78, 1, -2, 3, -4);
    if (raw.size() != 38 || raw[0] != 12 || raw[8] != 0x01 || raw[9] != 0x01 ||
        raw[24] != 0 || raw[25] != 0 || raw[26] != 0x55 || raw[27] != 0 ||
        raw[28] != 0 || raw[29] != 0 || raw[30] != 0x08 || raw[37] != 0x01) {
        return false;
    }

    const auto reliable = WrapReliableGamepad(3, timestamp, raw);
    if (reliable.size() != 50 || reliable[0] != 0x23 || reliable[9] != 0x21 ||
        reliable[10] != 0 || reliable[11] != 38 || reliable[12] != 12) {
        return false;
    }

    const auto partial = WrapPartiallyReliableGamepad(3, timestamp, 0, 1, raw);
    if (partial.size() != 54 || partial[0] != 0x23 || partial[9] != 0x26 ||
        partial[10] != 0 || partial[11] != 0 || partial[12] != 1 ||
        partial[13] != 0x21 || partial[14] != 0 || partial[15] != 38 || partial[16] != 12) {
        return false;
    }

    const auto mouse = BuildMouseButtonPayload(timestamp, true);
    const auto wrapped_mouse = WrapSingleInput(3, timestamp, mouse);
    const auto move = BuildMouseMovePayload(timestamp, 10, -5);
    const auto wrapped_move = WrapReliableGamepad(3, timestamp, move);
    const auto key = BuildKeyboardPayload(timestamp, 0x41, 0x1e, 0, true);
    return mouse.size() == 18 && mouse[0] == 8 && mouse[4] == 1 && mouse[17] == 0x08 &&
           wrapped_mouse.size() == 28 && wrapped_mouse[0] == 0x23 &&
           wrapped_mouse[9] == 0x22 && wrapped_mouse[10] == 8 &&
           move.size() == 22 && move[0] == 7 && move[4] == 0 && move[5] == 10 &&
           move[6] == 0xff && move[7] == 0xfb && wrapped_move.size() == 34 &&
           wrapped_move[9] == 0x21 && wrapped_move[10] == 0 && wrapped_move[11] == 22 &&
           key.size() == 18 && key[0] == 3 && key[4] == 0 && key[5] == 0x41 &&
           key[8] == 0 && key[9] == 0x1e;
}

} // namespace opennow::webrtc::internal

bool WebRtcSession::send_gamepad_input(
    uint16_t buttons,
    uint8_t left_trigger,
    uint8_t right_trigger,
    float lx,
    float ly,
    float rx,
    float ry) {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    gamepad_input_attempt_count_++;
    const PeerConnectionState peer_state = pc_ ? peer_connection_get_state(pc_) : PEER_CONNECTION_CLOSED;
    if (!pc_ || peer_state != PEER_CONNECTION_COMPLETED || !input_ready_) {
        gamepad_input_blocked_count_++;
        if (gamepad_input_blocked_count_ <= 8 || gamepad_input_blocked_count_ % 50 == 0) {
            AppendInputLog("PAD blocked attempt=" + std::to_string(gamepad_input_attempt_count_) +
                           " pc=" + std::to_string(pc_ ? 1 : 0) +
                           " peer=" + (pc_ ? std::string(peer_connection_state_to_string(peer_state)) : "none") +
                           " sctpOpen=" + std::to_string(datachannel_opened_ ? 1 : 0) +
                           " channelRequested=" + std::to_string(datachannel_open_requested_ ? 1 : 0) +
                           " inputReady=" + std::to_string(input_ready_ ? 1 : 0) +
                           " buttons=0x" + HexPreview(reinterpret_cast<const uint8_t*>(&buttons), sizeof(buttons), 2));
        }
        return false;
    }

    const uint64_t timestamp_us = NowUs();
    const std::vector<uint8_t> payload = BuildGamepadPayload(
        timestamp_us, buttons, left_trigger, right_trigger,
        AxisToI16(lx), AxisToI16(-ly), AxisToI16(rx), AxisToI16(-ry));

    const bool use_partial = partial_input_channel_requested_ && input_protocol_version_ >= 3;
    std::vector<uint8_t> wire_payload = use_partial
        ? WrapPartiallyReliableGamepad(input_protocol_version_, timestamp_us, 0, gamepad_sequence_++, payload)
        : WrapReliableGamepad(input_protocol_version_, timestamp_us, payload);
    const uint16_t sid = use_partial ? 2 : 0;
    const int sent = send_datachannel_binary(sid, "gamepad", wire_payload.data(), wire_payload.size());
    if (sent < 0)
        gamepad_send_failure_count_++;
    else
        gamepad_tx_count_++;
    if (gamepad_input_attempt_count_ <= 12 || gamepad_tx_count_ <= 8 ||
        gamepad_input_attempt_count_ % 100 == 0 || sent < 0) {
        AppendInputLog("PAD tx attempt=" + std::to_string(gamepad_input_attempt_count_) +
                       " report=" + std::to_string(gamepad_tx_count_) +
                       " sid=" + std::to_string(sid) +
                       " protocol=" + std::to_string(input_protocol_version_) +
                       " buttons=" + std::to_string(buttons) +
                       " lt=" + std::to_string(left_trigger) +
                       " rt=" + std::to_string(right_trigger) +
                       " axes=" + std::to_string(AxisToI16(lx)) + "/" +
                       std::to_string(AxisToI16(-ly)) + "/" +
                       std::to_string(AxisToI16(rx)) + "/" +
                       std::to_string(AxisToI16(-ry)) +
                       " wireBytes=" + std::to_string(wire_payload.size()) +
                       " sent=" + std::to_string(sent) +
                       " hex=" + HexPreview(wire_payload.data(), wire_payload.size(), 32));
    }
    return sent >= 0;
}

void WebRtcSession::send_mouse_left_button(bool pressed) {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    if (!pc_ || !input_ready_ || peer_connection_get_state(pc_) != PEER_CONNECTION_COMPLETED) {
        AppendInputLog("MOUSE blocked pressed=" + std::to_string(pressed ? 1 : 0) +
                       " pc=" + std::to_string(pc_ ? 1 : 0) +
                       " inputReady=" + std::to_string(input_ready_ ? 1 : 0));
        return;
    }

    const uint64_t timestamp_us = NowUs();
    const std::vector<uint8_t> payload = BuildMouseButtonPayload(timestamp_us, pressed);
    std::vector<uint8_t> wire_payload = WrapSingleInput(input_protocol_version_, timestamp_us, payload);
    const int sent = send_datachannel_binary(0, pressed ? "mouse_left_down" : "mouse_left_up",
                                             wire_payload.data(), wire_payload.size());
    if (sent >= 0)
        mouse_tx_count_++;
    AppendInputLog("MOUSE tx pressed=" + std::to_string(pressed ? 1 : 0) +
                   " sent=" + std::to_string(sent) +
                   " bytes=" + std::to_string(wire_payload.size()) +
                   " hex=" + HexPreview(wire_payload.data(), wire_payload.size(), 32));
}

void WebRtcSession::send_mouse_move(int16_t dx, int16_t dy) {
    constexpr int16_t kSafeMouseDelta = 4096;
    const int16_t safe_dx = std::clamp(dx, static_cast<int16_t>(-kSafeMouseDelta), kSafeMouseDelta);
    const int16_t safe_dy = std::clamp(dy, static_cast<int16_t>(-kSafeMouseDelta), kSafeMouseDelta);
    if (safe_dx != dx || safe_dy != dy) {
        AppendInputLog("MOUSE_MOVE clamped requested=" + std::to_string(dx) + "/" +
                       std::to_string(dy) + " safe=" + std::to_string(safe_dx) + "/" +
                       std::to_string(safe_dy));
    }
    dx = safe_dx;
    dy = safe_dy;
    if (dx == 0 && dy == 0)
        return;

    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    if (!pc_ || !input_ready_ || peer_connection_get_state(pc_) != PEER_CONNECTION_COMPLETED) {
        AppendInputLog("MOUSE_MOVE blocked dx=" + std::to_string(dx) +
                       " dy=" + std::to_string(dy));
        return;
    }

    const uint64_t timestamp_us = NowUs();
    const auto payload = BuildMouseMovePayload(timestamp_us, dx, dy);
    const auto wire_payload = input_protocol_version_ <= 2
        ? payload
        : WrapReliableGamepad(input_protocol_version_, timestamp_us, payload);
    const int sent = send_datachannel_binary(0, "mouse_move", wire_payload.data(), wire_payload.size());
    AppendInputLog("MOUSE_MOVE tx dx=" + std::to_string(dx) +
                   " dy=" + std::to_string(dy) + " sent=" + std::to_string(sent));
}

void WebRtcSession::send_keyboard_key(uint16_t keycode, uint16_t scancode,
                                      uint16_t modifiers, bool pressed) {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    if (!pc_ || !input_ready_ || peer_connection_get_state(pc_) != PEER_CONNECTION_COMPLETED) {
        if (!opennow::SensitiveInputLoggingSuppressed())
            AppendInputLog("KEY blocked vk=" + std::to_string(keycode) +
                           " pressed=" + std::to_string(pressed ? 1 : 0));
        return;
    }

    const uint64_t timestamp_us = NowUs();
    const auto payload = BuildKeyboardPayload(timestamp_us, keycode, scancode, modifiers, pressed);
    const auto wire_payload = WrapSingleInput(input_protocol_version_, timestamp_us, payload);
    const int sent = send_datachannel_binary(0, pressed ? "key_down" : "key_up",
                                             wire_payload.data(), wire_payload.size());
    if (!opennow::SensitiveInputLoggingSuppressed())
        AppendInputLog("KEY tx vk=" + std::to_string(keycode) +
                       " scan=" + std::to_string(scancode) +
                       " mods=" + std::to_string(modifiers) +
                       " pressed=" + std::to_string(pressed ? 1 : 0) +
                       " sent=" + std::to_string(sent));
}


void WebRtcSession::on_datachannel_message(const char* msg, size_t len, uint16_t sid) {
    std::string preview;
    if (msg && len > 0)
        preview.assign(msg, msg + std::min<size_t>(len, 180));

    const char* label_raw = pc_ ? peer_connection_lookup_sid_label(pc_, sid) : nullptr;
    const std::string label = label_raw ? label_raw : "";
    AppendStreamLog("DATA rx sid=" + std::to_string(sid) +
                    " label=" + (label.empty() ? std::string("(unknown)") : label) +
                    " bytes=" + std::to_string(len) +
                    " hex=" + HexPreview(reinterpret_cast<const uint8_t*>(msg), len, 16) +
                    " preview=" + PreviewText(preview, 120));
    AppendInputLog("RX sid=" + std::to_string(sid) +
                   " label=" + (label.empty() ? std::string("unknown") : label) +
                   " bytes=" + std::to_string(len) +
                   " hex=" + HexPreview(reinterpret_cast<const uint8_t*>(msg), len, 24));

    if (msg && len >= 2 && (label.empty() || label == "input_channel_v1" || sid == 0)) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(msg);
        const uint16_t first_word = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
        int version = 2;
        bool handshake = false;

        if (first_word == 526) {
            version = len >= 4 ? static_cast<int>(bytes[2] | (bytes[3] << 8)) : 2;
            handshake = true;
        } else if (bytes[0] == 0x0e) {
            version = first_word;
            handshake = true;
        }

        if (handshake) {
            input_ready_ = true;
            input_protocol_version_ = std::max(2, version);
            AppendStreamLog("DATA input_handshake_complete version=" +
                            std::to_string(input_protocol_version_) +
                            " firstWord=" + std::to_string(first_word));
            AppendInputLog("HANDSHAKE complete protocol=" + std::to_string(input_protocol_version_) +
                           " firstWord=" + std::to_string(first_word));
            maybe_send_input_heartbeat();
        }
    }
}

void WebRtcSession::on_datachannel_open() {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    datachannel_opened_ = true;
    consecutive_input_send_failures_ = 0;
    AppendStreamLog("DATA sctp_open attempts=" + std::to_string(datachannel_open_attempts_) +
                    " requested=" + std::to_string(datachannel_open_requested_ ? 1 : 0));
    AppendInputLog("SCTP associationOpen=1 attempts=" + std::to_string(datachannel_open_attempts_));
}

void WebRtcSession::on_datachannel_close() {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    AppendStreamLog("DATA sctp_close");
    AppendInputLog("SCTP associationOpen=0 reason=callback_close");
    reset_input_channel_for_retry("sctp_close");
}

int WebRtcSession::next_ack_id() {
    ack_counter_ += 1;
    return ack_counter_;
}

void WebRtcSession::send_peer_info() {
    if (!signaling_client_)
        return;

    json_t* root = json_object();
    json_object_set_new(root, "ackid", json_integer(next_ack_id()));

    json_t* info = json_object();
    json_object_set_new(info, "browser", json_string("Chrome"));
    json_object_set_new(info, "browserVersion", json_string("131"));
    json_object_set_new(info, "connected", json_true());
    json_object_set_new(info, "id", json_integer(peer_id_));
    json_object_set_new(info, "name", json_string(peer_name_.c_str()));
    json_object_set_new(info, "peerRole", json_integer(0));
    const std::string resolution = std::to_string(settings_.width) + "x" + std::to_string(settings_.height);
    json_object_set_new(info, "resolution", json_string(resolution.c_str()));
    json_object_set_new(info, "version", json_integer(2));
    json_object_set_new(root, "peer_info", info);

    char* dump = json_dumps(root, 0);
    if (dump) {
        AppendStreamLog("TX " + std::string(dump));
        signaling_client_->send_message(std::string(dump));
        free(dump);
    }
    last_peer_info_sent_ = std::chrono::steady_clock::now();
    json_decref(root);
}

void WebRtcSession::send_peer_payload(json_t* payload) {
    if (!signaling_client_ || !payload)
        return;

    char* payload_dump = json_dumps(payload, 0);
    if (!payload_dump)
        return;

    json_t* root = json_object();
    json_t* peer_msg = json_object();
    json_object_set_new(peer_msg, "from", json_integer(peer_id_));
    json_object_set_new(peer_msg, "to", json_integer(remote_peer_id_));
    json_object_set_new(peer_msg, "msg", json_string(payload_dump));
    json_object_set_new(root, "peer_msg", peer_msg);
    json_object_set_new(root, "ackid", json_integer(next_ack_id()));

    char* dump = json_dumps(root, 0);
    if (dump) {
        AppendStreamLog("TX " + std::string(dump));
        AppendTraceLog("TX peer_payload " + PreviewText(std::string(payload_dump), 500));
        signaling_client_->send_message(std::string(dump));
        free(dump);
    }

    free(payload_dump);
    json_decref(root);
}

void WebRtcSession::send_heartbeat() {
    if (!signaling_client_)
        return;

    json_t* heartbeat = json_object();
    json_object_set_new(heartbeat, "hb", json_integer(1));
    char* dump = json_dumps(heartbeat, 0);
    if (dump) {
        heartbeat_tx_count_++;
        AppendStreamLog("TX " + std::string(dump));
        signaling_client_->send_message(std::string(dump));
        free(dump);
    }
    json_decref(heartbeat);
    last_heartbeat_sent_ = std::chrono::steady_clock::now();
}

void WebRtcSession::maybe_send_keepalive() {
    if (!signaling_ready_ || !signaling_client_)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (last_heartbeat_sent_.time_since_epoch().count() == 0 ||
        now - last_heartbeat_sent_ >= std::chrono::seconds(5)) {
        send_heartbeat();
    }

    if (!offer_received_ &&
        (last_peer_info_sent_.time_since_epoch().count() == 0 ||
         now - last_peer_info_sent_ >= std::chrono::seconds(2))) {
        send_peer_info();
    }
}


void WebRtcSession::maybe_open_datachannel() {
    if (!pc_ || datachannel_open_requested_)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (last_datachannel_open_attempt_.time_since_epoch().count() != 0 &&
        now - last_datachannel_open_attempt_ < std::chrono::milliseconds(750)) {
        return;
    }

    if (datachannel_open_attempts_ >= 60)
        return;

    last_datachannel_open_attempt_ = now;
    datachannel_open_attempts_++;

    char protocol[] = "";

    if (!reliable_input_channel_requested_) {
        char reliable_label[] = "input_channel_v1";
        const int ret = peer_connection_create_datachannel_sid(
            pc_,
            DATA_CHANNEL_RELIABLE,
            0,
            0,
            reliable_label,
            protocol,
            0);

        AppendStreamLog("DATA create_channel label=input_channel_v1 sid=0 type=reliable ret=" +
                        std::to_string(ret) +
                        " attempt=" + std::to_string(datachannel_open_attempts_));
        AppendInputLog("DCEP create sid=0 label=input_channel_v1 attempt=" +
                       std::to_string(datachannel_open_attempts_) +
                       " result=" + std::to_string(ret) +
                       " sctpOpen=" + std::to_string(datachannel_opened_ ? 1 : 0));

        if (ret >= 0)
            reliable_input_channel_requested_ = true;
    }

    // The Switch fallback SCTP backend intentionally uses one reliable channel.
    // Partial reliability requires Forward-TSN negotiation and is optional for GFN input.
    datachannel_open_requested_ = reliable_input_channel_requested_;
    if (datachannel_open_requested_ && input_activation_due_.time_since_epoch().count() == 0) {
        // Prefer the server handshake; use v2 only as a compatibility fallback.
        input_activation_due_ = now + std::chrono::milliseconds(1500);
        AppendStreamLog("DATA fast_channel disabled reason=internal_sctp_no_forward_tsn");
        AppendInputLog("DCEP reliableChannelRequested=1 fastChannel=disabled activationDelayMs=1500");
    }
}

void WebRtcSession::reset_input_channel_for_retry(const char* reason) {
    datachannel_opened_ = false;
    datachannel_open_requested_ = false;
    reliable_input_channel_requested_ = false;
    partial_input_channel_requested_ = false;
    input_ready_ = false;
    startup_control_sent_ = false;
    input_activation_due_ = {};
    last_datachannel_open_attempt_ = {};
    last_input_heartbeat_sent_ = {};
    consecutive_input_send_failures_ = 0;
    AppendInputLog(std::string("RECOVERY input_channel_reset reason=") +
                   (reason ? reason : "unknown") +
                   " attempts=" + std::to_string(datachannel_open_attempts_));
}

void WebRtcSession::note_input_send_result(int sent, const char* label) {
    if (sent >= 0) {
        consecutive_input_send_failures_ = 0;
        return;
    }

    consecutive_input_send_failures_++;
    if (consecutive_input_send_failures_ < 3)
        return;

    // Do not keep acknowledging controller states locally while SCTP rejects
    // them. Pause briefly, then reactivate the negotiated reliable channel.
    input_ready_ = false;
    input_activation_due_ = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(250);
    AppendInputLog(std::string("RECOVERY input_send_failures label=") +
                   (label ? label : "unknown") +
                   " consecutive=" + std::to_string(consecutive_input_send_failures_) +
                   " action=reactivate");
    consecutive_input_send_failures_ = 0;
}

void WebRtcSession::maybe_activate_input() {
    if (input_ready_ || !datachannel_open_requested_ || !datachannel_opened_ || !pc_)
        return;

    if (input_activation_due_.time_since_epoch().count() == 0 ||
        std::chrono::steady_clock::now() < input_activation_due_)
        return;

    input_ready_ = true;
    input_protocol_version_ = 2;
    AppendStreamLog("DATA input_ready fallback=v2 controller=xbox sidReliable=0 sidFast=2");
    AppendInputLog("HANDSHAKE fallback protocol=2 inputReady=1 controller=xbox sid=0");
}

void WebRtcSession::send_datachannel_text(const std::string& label, const std::string& payload) {
    if (!pc_ || payload.empty())
        return;

    const int sent = peer_connection_datachannel_send(pc_, const_cast<char*>(payload.c_str()), payload.size());
    AppendStreamLog("DATA tx label=" + label +
                    " opened=" + std::to_string(datachannel_opened_ ? 1 : 0) +
                    " sent=" + std::to_string(sent) +
                    " bytes=" + std::to_string(payload.size()) +
                    " payload=" + PreviewText(payload, 220));
}

int WebRtcSession::send_datachannel_binary(uint16_t sid, const std::string& label, const uint8_t* payload, size_t size) {
    if (!pc_ || !payload || size == 0)
        return -1;

    const int sent = peer_connection_datachannel_send_binary_sid(
        pc_,
        const_cast<char*>(reinterpret_cast<const char*>(payload)),
        size,
        sid);
    note_input_send_result(sent, label.c_str());
    // Controller reports are sent every frame; logging each one would create
    // input latency and make the useful SCTP diagnostics unreadable.
    const bool high_frequency_input = label == "gamepad" || label == "input_heartbeat";
    if (!high_frequency_input || gamepad_tx_count_ < 5 || gamepad_tx_count_ % 120 == 0) {
        AppendStreamLog("DATA tx-binary label=" + label +
                        " sid=" + std::to_string(sid) +
                        " sent=" + std::to_string(sent) +
                        " bytes=" + std::to_string(size) +
                        " hex=" + HexPreview(payload, size, 16));
    }
    return sent;
}

void WebRtcSession::maybe_send_input_heartbeat() {
    if (!pc_ || !input_ready_ || !reliable_input_channel_requested_)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (last_input_heartbeat_sent_.time_since_epoch().count() != 0 &&
        now - last_input_heartbeat_sent_ < std::chrono::seconds(2)) {
        return;
    }

    last_input_heartbeat_sent_ = now;
    input_heartbeat_tx_count_++;

    uint8_t heartbeat[4] = {2, 0, 0, 0}; // INPUT_HEARTBEAT, little-endian.
    const int sent = send_datachannel_binary(
        0, "input_heartbeat", heartbeat, sizeof(heartbeat));

    if (input_heartbeat_tx_count_ <= 5 || input_heartbeat_tx_count_ % 20 == 0) {
        AppendStreamLog("DATA input_heartbeat sent=" + std::to_string(sent) +
                        " count=" + std::to_string(input_heartbeat_tx_count_) +
                        " protocol=" + std::to_string(input_protocol_version_));
        AppendInputLog("HEARTBEAT tx count=" + std::to_string(input_heartbeat_tx_count_) +
                       " sent=" + std::to_string(sent) +
                       " protocol=" + std::to_string(input_protocol_version_));
    }
}

void WebRtcSession::maybe_send_startup_control_messages() {
    if (!pc_ || startup_control_sent_ || !datachannel_open_requested_)
        return;

    startup_control_sent_ = true;
    AppendStreamLog("DATA startup_channels requested reliable=" +
                    std::to_string(reliable_input_channel_requested_ ? 1 : 0) +
                    " partial=" + std::to_string(partial_input_channel_requested_ ? 1 : 0) +
                    " thresholdMs=" + std::to_string(partial_reliable_threshold_ms_));
}
