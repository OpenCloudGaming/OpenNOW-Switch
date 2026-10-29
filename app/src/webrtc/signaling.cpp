#include "webrtc_session.hpp"
#include "internal.hpp"
#include "startup_timeout_policy.hpp"

using namespace opennow::webrtc::internal;

namespace
{

std::string EncodeQueryValue(const std::string& value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char byte : value) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            encoded += static_cast<char>(byte);
        } else {
            encoded += '%';
            encoded += hex[byte >> 4];
            encoded += hex[byte & 15];
        }
    }
    return encoded;
}

std::string BuildSignInUrl(std::string base_url, const std::string& peer_name,
                         const std::string& session_id, bool reconnect)
{
    const size_t query_pos = base_url.find_first_of("?#");
    if (query_pos != std::string::npos)
        base_url.erase(query_pos);

    if (StartsWith(base_url, "http://"))
        base_url.replace(0, 4, "ws");
    else if (StartsWith(base_url, "https://"))
        base_url.replace(0, 5, "wss");

    while (!base_url.empty() && base_url.back() == '/')
        base_url.pop_back();

    if (!base_url.ends_with("/sign_in"))
        base_url += "/sign_in";

    return base_url + "?peer_id=" + EncodeQueryValue(peer_name) +
        (reconnect ? "&version=3.0" : "&version=2") + "&peer_role=1&pairing_id=" +
        EncodeQueryValue(session_id) + (reconnect ? "&reconnect=1" : "");
}

}

void WebRtcSession::connect_signaling()
{
    const auto deadline = session_started_at_ + opennow::webrtc::TransportStartupTimeout;
    if (stop_requested_.load(std::memory_order_acquire) || offer_received_)
        return;
    if (std::chrono::steady_clock::now() >= deadline) {
        handle_signaling_failure(WebSocketClient::FailureKind::Terminal);
        return;
    }
    signaling_retry_due_ = {};
    signaling_server_rejected_ = false;
    peer_id_ = 0;
    remote_peer_id_ = 1;
    ack_counter_ = 0;
    const std::string sign_in_url = BuildSignInUrl(
        signaling_url_, peer_name_, session_id_, signaling_retries_ > 0);
    signaling_client_ = std::make_unique<SignalingClient>(sign_in_url);
    signaling_client_->set_on_message([this](const std::string& msg) {
        handle_signaling_message(msg);
    });

    std::vector<std::string> headers;
    headers.push_back("Origin: https://play.geforcenow.com");
    headers.push_back("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/131.0.0.0 Safari/537.36");
    if (!session_id_.empty()) {
        headers.push_back("Sec-WebSocket-Protocol: x-nv-sessionid." + session_id_);
    }
    signaling_client_->set_custom_headers(headers);

    if (!signaling_client_->connect(deadline)) {
        handle_signaling_failure(signaling_client_->failure_kind());
        return;
    }
    signaling_ready_ = true;
    current_state_ = "Signaling connected, waiting for offer";
    AppendStreamLog("SIGNAL connected");
    send_peer_info();
    send_heartbeat();
}

void WebRtcSession::poll_signaling()
{
    if (stop_requested_.load(std::memory_order_acquire))
        return;
    if (signaling_retry_due_.time_since_epoch().count() != 0) {
        if (std::chrono::steady_clock::now() >= signaling_retry_due_)
            connect_signaling();
        return;
    }
    if (signaling_client_) {
        signaling_client_->poll();
        if (signaling_server_rejected_ || !signaling_client_->is_connected()) {
            handle_signaling_failure(signaling_server_rejected_
                ? WebSocketClient::FailureKind::Terminal : signaling_client_->failure_kind());
            return;
        }
    }
    maybe_send_keepalive();
}

void WebRtcSession::handle_signaling_failure(WebSocketClient::FailureKind failure)
{
    const std::string detail = signaling_server_rejected_ ? "Signaling server rejected request"
        : signaling_client_ ? signaling_client_->get_last_error() : "Signaling startup deadline expired";
    AppendStreamLog("SIGNAL failure " + detail);
    signaling_client_.reset();
    signaling_ready_ = offer_received_;
    if (offer_received_)
        return;

    const auto now = std::chrono::steady_clock::now();
    const auto delay = std::chrono::seconds(1 << signaling_retries_);
    if (failure == WebSocketClient::FailureKind::Transient && signaling_retries_ < 3 &&
        now + delay < session_started_at_ + opennow::webrtc::TransportStartupTimeout) {
        signaling_retries_++;
        signaling_retry_due_ = now + delay;
        current_state_ = "Retrying signaling (" + std::to_string(signaling_retries_) + "/3)";
        AppendStreamLog("SIGNAL retry scheduled attempt=" + std::to_string(signaling_retries_));
        return;
    }

    signaling_retry_due_ = {};
    current_state_ = detail;
    peer_terminal_kind_.store(static_cast<int>(opennow::PeerTerminalKind::Failed), std::memory_order_release);
    peer_terminal_.store(true, std::memory_order_release);
    request_stop();
}
