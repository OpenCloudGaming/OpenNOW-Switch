#include "signaling_client.hpp"

SignalingClient::SignalingClient(const std::string& url) : url_(url) {
    ws_ = std::make_unique<WebSocketClient>(url_);
}

SignalingClient::~SignalingClient() {
    if (ws_) {
        ws_->disconnect();
    }
}

bool SignalingClient::connect() {
    if (!ws_) return false;

    ws_->set_on_message([this](const std::string& msg) {
        if (on_message_) {
            on_message_(msg);
        }
    });
    
    if (ws_->connect()) {
        return true;
    }
    return false;
}

void SignalingClient::poll() {
    if (ws_) {
        ws_->poll();
    }
}

void SignalingClient::send_message(const std::string& msg) {
    if (ws_) {
        ws_->send_message(msg);
    }
}
