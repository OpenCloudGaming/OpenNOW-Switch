#pragma once

#include <string>
#include <vector>
#include <functional>
#include <curl/curl.h>

class WebSocketClient {
public:
    using MessageCallback = std::function<void(const std::string&)>;

    WebSocketClient(const std::string& url);
    ~WebSocketClient();

    bool connect();
    void disconnect();
    void poll();
    void send_message(const std::string& msg);

    void set_on_message(MessageCallback cb) { on_message_ = cb; }
    void set_custom_headers(const std::vector<std::string>& headers) { custom_headers_ = headers; }
    bool is_connected() const { return connected_; }
    std::string get_last_error() const { return last_error_; }

private:
    std::string url_;
    std::vector<std::string> custom_headers_;
    CURL* curl_ = nullptr;
    MessageCallback on_message_;
    bool connected_ = false;
    std::string last_error_;
    std::vector<uint8_t> rx_buffer_;

    bool send_all(const uint8_t* data, size_t length);
    bool send_frame(uint8_t opcode, const uint8_t* payload, size_t length);
};
