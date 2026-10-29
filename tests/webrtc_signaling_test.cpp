#include "websocket_test_transport.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <jansson.h>

#define private public
#include "webrtc_session.hpp"
#undef private
#include "webrtc/internal.hpp"

static std::vector<std::string> stream_logs;
static int remote_descriptions = 0;
static int answers_created = 0;

extern "C" {
int peer_init() { return 0; }
void peer_deinit() {}
void peer_connection_set_remote_description(PeerConnection*, const char*, SdpType) { ++remote_descriptions; }
const char* peer_connection_create_answer(PeerConnection*) { ++answers_created; return nullptr; }
int peer_connection_add_ice_candidate(PeerConnection*, char*) { return 0; }
PeerConnectionState peer_connection_get_state(PeerConnection*) { return PEER_CONNECTION_COMPLETED; }
const char* peer_connection_state_to_string(PeerConnectionState) { return "completed"; }
}

namespace opennow {
bool StreamDiagnosticsEnabled() { return false; }
}

namespace opennow::webrtc::internal {
bool StartsWith(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }
void AppendStreamLog(const std::string& line) { stream_logs.push_back(line); }
void AppendInputLog(const std::string&) {}
void AppendTraceLog(const std::string&) {}
void AppendSdpSummary(const std::string&, const std::string&) {}
}

WebRtcSession::WebRtcSession(const std::string& url, const std::string&, const std::string& id,
                             const std::string&, int, const std::vector<opennow::IceServerInfo>&,
                             const opennow::StreamSettings& settings)
    : signaling_url_(url), session_id_(id), settings_(settings), peer_name_("peer-test") {
    session_started_at_ = std::chrono::steady_clock::now();
}
WebRtcSession::~WebRtcSession() = default;
void WebRtcSession::log_stream_summary(const char*) {}
void WebRtcSession::request_stop() { stop_requested_.store(true); }
FFmpegVideoDecoder::~FFmpegVideoDecoder() = default;
int FFmpegVideoDecoder::setup(int, int, int, int, void*, int) { return 0; }
void FFmpegVideoDecoder::cleanup() {}
int FFmpegVideoDecoder::submit_decode_unit(uint8_t*, int, int64_t) { return 0; }
int FFmpegVideoDecoder::capabilities() const { return 0; }
VideoDecodeStats* FFmpegVideoDecoder::video_decode_stats() { return nullptr; }
struct AudioPipeline::Impl {};
AudioPipeline::~AudioPipeline() = default;

static void deliver(WebRtcSession& session, const std::string& message, uint8_t opcode = 0x81) {
    auto& fake = latest();
    assert(message.size() < 126);
    fake.input = {opcode, static_cast<uint8_t>(message.size())};
    fake.input.insert(fake.input.end(), message.begin(), message.end());
    session.poll_signaling();
    session.poll_signaling();
}

static void close_socket(WebRtcSession& session, uint16_t code) {
    latest().input = {0x88, 2, static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code)};
    session.poll_signaling();
    session.poll_signaling();
}

static void retry_now(WebRtcSession& session) {
    assert(session.signaling_retry_due_ > std::chrono::steady_clock::now());
    const auto dials = transports.size();
    session.poll_signaling();
    assert(transports.size() == dials);
    session.signaling_retry_due_ = std::chrono::steady_clock::now();
    session.poll_signaling();
}

int main(int argc, char** argv) {
    const std::string scenario = argc > 1 ? argv[1] : "url";
    if (scenario == "url") {
        for (const auto* url : {"wss://rig.invalid/nvst/", "wss://rig.invalid/nvst/sign_in",
                                "https://rig.invalid/nvst/sign_in/?old=1#fragment"}) {
            WebRtcSession session(url, "", "test-session", "", 0, {}, {});
            session.connect_signaling();
            assert(session.signaling_ready_);
            assert(latest().request.starts_with("GET /nvst/sign_in?peer_id=peer-test&version=2&peer_role=1&pairing_id=test-session HTTP/1.1\r\n"));
            assert(latest().request.find("Sec-WebSocket-Protocol: x-nv-sessionid.test-session\r\n") != std::string::npos);
        }
        WebRtcSession session("wss://rig.invalid/nvst/sign_in#fragment", "", "s&?/#=+%", "", 0, {}, {});
        session.peer_name_ = "peer &?/#=+%\xc3\xa9";
        session.connect_signaling();
        assert(latest().request.starts_with("GET /nvst/sign_in?peer_id=peer%20%26%3F%2F%23%3D%2B%25%C3%A9&version=2&peer_role=1&pairing_id=s%26%3F%2F%23%3D%2B%25 HTTP/1.1\r\n"));
    } else if (scenario == "rejected") {
        for (const auto* message : {R"({"error":"peerRemoved"})", R"({"error":{"secret":"never-log-this"}})"}) {
            WebRtcSession session("wss://rig.invalid/nvst/", "", "test-session", "", 0, {}, {});
            session.connect_signaling();
            deliver(session, message);
            assert(session.is_terminal());
            assert(session.stop_requested_.load());
            assert(!session.signaling_ready_);
            assert(session.signaling_retries_ == 0);
            assert(session.current_state_ == "Signaling server rejected request");
        }
        for (const auto& line : stream_logs)
            assert(line.find("never-log-this") == std::string::npos);
        WebRtcSession session("wss://rig.invalid/nvst/", "", "test-session", "", 0, {}, {});
        session.connect_signaling();
        deliver(session, R"({"error":null,"hb":1})");
        assert(!session.is_terminal() && session.heartbeat_rx_count_ == 1);
    } else if (scenario == "closed") {
        WebRtcSession session("wss://rig.invalid/nvst/", "", "test-session", "", 0, {}, {});
        session.connect_signaling();
        close_socket(session, 1013);
        assert(!session.signaling_ready_);
        assert(!session.is_terminal());
        assert(session.signaling_retries_ == 1);
        assert(session.signaling_retry_due_ > std::chrono::steady_clock::now());
        const auto started = session.session_started_at_;
        retry_now(session);
        assert(session.session_started_at_ == started);
        assert(session.signaling_ready_ && transports.size() == 2);
        assert(latest().request.starts_with("GET /nvst/sign_in?peer_id=peer-test&version=3.0&peer_role=1&pairing_id=test-session&reconnect=1 HTTP/1.1\r\n"));
        deliver(session, R"({"ackid":9,"peer_info":{"name":"peer-test","id":42}})");
        assert(session.peer_id_ == 42);
        session.pc_ = reinterpret_cast<PeerConnection*>(&session);
        deliver(session, R"({"peer_msg":{"from":17,"msg":"{\"type\":\"offer\",\"sdp\":\"v=0\"}"}})", 0x82);
        assert(session.offer_count_ == 1 && session.remote_peer_id_ == 17);
        assert(remote_descriptions == 1 && answers_created == 1);
    } else if (scenario == "initial-transient") {
        for (bool http : {false, true}) {
            if (http) next_upgrade = UpgradeResponse::Unavailable;
            else next_perform_result = CURLE_COULDNT_CONNECT;
            WebRtcSession session("wss://rig.invalid/nvst/", "", "test-session", "", 0, {}, {});
            session.pc_ = reinterpret_cast<PeerConnection*>(&session);
            const auto started = session.session_started_at_;
            session.connect_signaling();
            assert(!session.is_terminal() && !session.signaling_ready_);
            assert(session.signaling_retries_ == 1);
            retry_now(session);
            assert(session.session_started_at_ == started);
            assert(session.pc_ == reinterpret_cast<PeerConnection*>(&session));
            assert(session.signaling_ready_);
            assert(latest().request.find("version=3.0") != std::string::npos);
            session.poll_signaling();
            const auto sent = frames(latest());
            assert(sent.size() == 2);
            json_error_t error;
            json_t* info = json_loads(sent.front().payload.c_str(), 0, &error);
            assert(info);
            json_t* peer = json_object_get(info, "peer_info");
            assert(std::string(json_string_value(json_object_get(peer, "name"))) == "peer-test");
            assert(json_integer_value(json_object_get(peer, "id")) == 0);
            json_decref(info);
        }
    } else if (scenario == "terminal") {
        for (int failure = 0; failure < 3; ++failure) {
            if (failure == 0) next_upgrade = UpgradeResponse::Unauthorized;
            else if (failure == 1) next_upgrade = UpgradeResponse::WrongAccept;
            else next_perform_result = CURLE_PEER_FAILED_VERIFICATION;
            WebRtcSession session("wss://rig.invalid/nvst/", "", "test-session", "", 0, {}, {});
            session.connect_signaling();
            assert(session.is_terminal() && !session.signaling_ready_);
            assert(session.signaling_retries_ == 0);
            assert(session.signaling_retry_due_.time_since_epoch().count() == 0);
            const auto dials = transports.size();
            for (int i = 0; i < 20; ++i) session.poll_signaling();
            assert(transports.size() == dials);
        }
    } else if (scenario == "retry-limit") {
        next_upgrade = UpgradeResponse::Unavailable;
        WebRtcSession session("wss://rig.invalid/nvst/", "", "test-session", "", 0, {}, {});
        session.connect_signaling();
        for (int retry = 1; retry <= 3; ++retry) {
            assert(session.signaling_retries_ == retry);
            next_upgrade = UpgradeResponse::Unavailable;
            retry_now(session);
        }
        assert(session.is_terminal());
        assert(transports.size() == 4);
    } else if (scenario == "deadline") {
        WebRtcSession session("wss://rig.invalid/nvst/", "", "test-session", "", 0, {}, {});
        session.connect_signaling();
        close_socket(session, 1013);
        session.session_started_at_ = std::chrono::steady_clock::now() - std::chrono::seconds(28);
        retry_now(session);
        assert(session.signaling_ready_);
        assert(latest().connect_timeout_ms > 0 && latest().connect_timeout_ms <= 2000);
        assert(latest().timeout_ms > 0 && latest().timeout_ms <= 2000);
        const auto dials = transports.size();
        close_socket(session, 1013);
        assert(session.is_terminal());
        assert(transports.size() == dials);

        WebRtcSession expired("wss://rig.invalid/nvst/", "", "test-session", "", 0, {}, {});
        expired.connect_signaling();
        close_socket(expired, 1013);
        const auto before = transports.size();
        expired.session_started_at_ = std::chrono::steady_clock::now() - std::chrono::seconds(31);
        retry_now(expired);
        assert(expired.is_terminal() && transports.size() == before);
    } else if (scenario == "post-offer") {
        for (int code : {4001, 1013, 1000, 1008, 0}) {
            WebRtcSession session("wss://rig.invalid/nvst/", "", "test-session", "", 0, {}, {});
            session.connect_signaling();
            session.pc_ = reinterpret_cast<PeerConnection*>(&session);
            deliver(session, R"({"peer_msg":{"from":1,"msg":"{\"type\":\"offer\",\"sdp\":\"v=0\"}"}})");
            assert(session.offer_received_ && session.remote_description_set_);
            session.peer_completed_seen_ = true;
            session.frames_decoded_.store(1);
            const auto dials = transports.size();
            const auto answers = answers_created;
            if (code == 0) deliver(session, R"({"error":"peerRemoved"})");
            else close_socket(session, static_cast<uint16_t>(code));
            const auto heartbeats = session.heartbeat_tx_count_;
            for (int i = 0; i < 200; ++i) session.poll_signaling();
            assert(session.signaling_ready_ && !session.is_terminal() && !session.stop_requested_.load());
            assert(!session.signaling_client_);
            assert(session.heartbeat_tx_count_ == heartbeats);
            assert(session.signaling_retries_ == 0 && transports.size() == dials);
            assert(answers_created == answers);
            assert(session.pc_ == reinterpret_cast<PeerConnection*>(&session));
        }
    } else {
        assert(false);
    }
}
