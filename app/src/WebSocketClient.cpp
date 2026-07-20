#include "WebSocketClient.hpp"
#include <iostream>
#include <borealis.hpp>
#include <array>
#include <cstring>
#include <cstdint>
#include <limits>
#include <random>
#include <thread>
#include <chrono>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace
{

std::vector<uint8_t> random_bytes(size_t length)
{
    std::vector<uint8_t> bytes(length);

#ifdef __SWITCH__
    randomGet(bytes.data(), bytes.size());
#else
    static thread_local std::mt19937 rng([] {
        std::random_device rd;
        const auto tick_seed = static_cast<uint32_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        return tick_seed ^ rd();
    }());

    std::uniform_int_distribution<int> dist(0, 255);
    for (uint8_t& byte : bytes)
        byte = static_cast<uint8_t>(dist(rng));
#endif

    return bytes;
}

std::string base64_encode(const uint8_t* data, size_t length) {
    std::string out;
    int val = 0, valb = -6;
    for (size_t i = 0; i < length; ++i) {
        uint8_t c = data[i];
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

bool starts_with(const std::string& value, const char* prefix)
{
    return value.rfind(prefix, 0) == 0;
}

bool is_successful_handshake(const std::string& response)
{
    const size_t line_end = response.find("\r\n");
    const std::string status_line =
        line_end == std::string::npos ? response : response.substr(0, line_end);

    return status_line.find(" 101 ") != std::string::npos ||
           status_line.find(" 101\r") != std::string::npos ||
           (status_line.size() >= 4 && status_line.compare(status_line.size() - 4, 4, " 101") == 0);
}

} // namespace

WebSocketClient::WebSocketClient(const std::string& url) : url_(url) {
    curl_ = curl_easy_init();
}

WebSocketClient::~WebSocketClient() {
    disconnect();
    if (curl_) curl_easy_cleanup(curl_);
}

bool WebSocketClient::connect() {
    if (!curl_) return false;
    if (connected_) disconnect();

    last_error_.clear();
    rx_buffer_.clear();

    // Parse URL for host and path (very basic parsing)
    std::string host = url_;
    std::string path = "/";
    size_t protocol_pos = host.find("://");
    if (protocol_pos != std::string::npos) {
        host = host.substr(protocol_pos + 3);
    }
    size_t path_pos = host.find("/");
    if (path_pos != std::string::npos) {
        path = host.substr(path_pos);
        host = host.substr(0, path_pos);
    }
    
    const std::string host_header = host;

    // Keep a host without default ports only for URL parsing internals. The
    // websocket Host header keeps the original host:port, matching NVIDIA's
    // web client behavior for nvst signaling.
    size_t colon_pos = host.find(":");
    if (colon_pos != std::string::npos) {
        std::string port_str = host.substr(colon_pos + 1);
        if (port_str == "443" || port_str == "80") {
            host = host.substr(0, colon_pos);
        }
    }

    // Convert wss:// to https:// or ws:// to http:// to let libcurl handle proxy/tls setup
    std::string curl_url = url_;
    if (starts_with(curl_url, "wss://")) curl_url.replace(0, 3, "https");
    else if (starts_with(curl_url, "ws://")) curl_url.replace(0, 2, "http");

    curl_easy_setopt(curl_, CURLOPT_URL, curl_url.c_str());
    curl_easy_setopt(curl_, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl_);
    if (res != CURLE_OK) {
        last_error_ = "Perform Failed: " + std::string(curl_easy_strerror(res));
        brls::Logger::error("WebSocket connect failed: {}", curl_easy_strerror(res));
        return false;
    }

    // Generate random key
    std::vector<uint8_t> key = random_bytes(16);
    std::string key_b64 = base64_encode(key.data(), key.size());

    std::string handshake = "GET " + path + " HTTP/1.1\r\n"
                            "Host: " + host_header + "\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: " + key_b64 + "\r\n"
                            "Sec-WebSocket-Version: 13\r\n";
    for (const auto& h : custom_headers_) {
        handshake += h + "\r\n";
    }
    handshake += "\r\n";

    if (!send_all(reinterpret_cast<const uint8_t*>(handshake.data()), handshake.size()))
        return false;

    // Receive handshake response
    char buf[1024];
    size_t rcvd = 0;
    std::string response;
    
    int retries = 50; // 5 seconds max
    while (retries > 0) {
        res = curl_easy_recv(curl_, buf, sizeof(buf) - 1, &rcvd);
        if (res == CURLE_OK && rcvd > 0) {
            response.append(buf, rcvd);
            const size_t header_end = response.find("\r\n\r\n");
            if (header_end == std::string::npos)
                continue;

            if (is_successful_handshake(response)) {
                const size_t body_start = header_end + 4;
                if (response.size() > body_start) {
                    rx_buffer_.insert(rx_buffer_.end(), response.begin() + body_start, response.end());
                }
                connected_ = true;
                brls::Logger::info("WebSocket connected!");
                return true;
            } else {
                last_error_ = "Invalid WS Response:\n" + response;
                brls::Logger::error("Invalid WS Response:\n{}", response);
                break;
            }
        } else if (res == CURLE_AGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            retries--;
        } else {
            last_error_ = "Recv error: " + std::string(curl_easy_strerror(res));
            brls::Logger::error("WebSocket recv error: {}", curl_easy_strerror(res));
            break;
        }
    }
    if (last_error_.empty()) last_error_ = "Handshake timeout";
    return false;
}

void WebSocketClient::disconnect() {
    if (connected_) {
        uint8_t close_payload[] = { 0x03, 0xe8 }; // Normal closure
        send_frame(0x88, close_payload, sizeof(close_payload));
        connected_ = false;
    }
}

bool WebSocketClient::send_all(const uint8_t* data, size_t length) {
    if (!curl_) return false;

    size_t total_sent = 0;
    int retry_budget = 100;
    while (total_sent < length) {
        size_t sent = 0;
        CURLcode res = curl_easy_send(curl_, data + total_sent, length - total_sent, &sent);
        if (res == CURLE_AGAIN) {
            if (--retry_budget <= 0) {
                last_error_ = "Send timeout";
                connected_ = false;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (res != CURLE_OK) {
            last_error_ = "Send error: " + std::string(curl_easy_strerror(res));
            connected_ = false;
            return false;
        }
        if (sent == 0) {
            last_error_ = "Send stalled";
            connected_ = false;
            return false;
        }

        total_sent += sent;
        retry_budget = 100;
    }

    return true;
}

bool WebSocketClient::send_frame(uint8_t opcode, const uint8_t* payload, size_t length) {
    if (!connected_ || !curl_) return false;

    std::vector<uint8_t> frame;
    frame.push_back(opcode | 0x80);

    // Client must mask frames
    uint8_t mask_bit = 0x80;
    if (length < 126) {
        frame.push_back(mask_bit | (uint8_t)length);
    } else if (length <= 0xFFFF) {
        frame.push_back(mask_bit | 126);
        frame.push_back((length >> 8) & 0xFF);
        frame.push_back(length & 0xFF);
    } else {
        frame.push_back(mask_bit | 127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((length >> (i * 8)) & 0xFF);
        }
    }

    std::vector<uint8_t> mask_key = random_bytes(4);
    for (uint8_t byte : mask_key) frame.push_back(byte);

    for (size_t i = 0; i < length; i++) {
        frame.push_back(payload[i] ^ mask_key[i % 4]);
    }

    return send_all(frame.data(), frame.size());
}

void WebSocketClient::send_message(const std::string& msg) {
    send_frame(0x81, (const uint8_t*)msg.data(), msg.length());
}

void WebSocketClient::poll() {
    if (!connected_ || !curl_) return;

    // Read all available data into rx_buffer_
    uint8_t temp[4096];
    while (true) {
        size_t rcvd = 0;
        CURLcode res = curl_easy_recv(curl_, temp, sizeof(temp), &rcvd);
        if (res == CURLE_OK && rcvd > 0) {
            rx_buffer_.insert(rx_buffer_.end(), temp, temp + rcvd);
        } else {
            if (res == CURLE_OK && rcvd == 0) {
                last_error_ = "Remote endpoint closed the signaling connection";
                connected_ = false;
            } else if (res != CURLE_AGAIN) {
                last_error_ = "Receive error: " + std::string(curl_easy_strerror(res));
                connected_ = false;
            }
            break;
        }
    }

    // Process all complete frames in the buffer
    while (rx_buffer_.size() >= 2) {
        uint8_t opcode = rx_buffer_[0] & 0x0F;
        uint8_t payload_len_7 = rx_buffer_[1] & 0x7F;
        bool masked = (rx_buffer_[1] & 0x80) != 0;
        
        size_t header_size = 2;
        size_t payload_len = payload_len_7;
        
        if (payload_len_7 == 126) {
            header_size += 2;
            if (rx_buffer_.size() < header_size) return; // Need more data
            payload_len = (static_cast<size_t>(rx_buffer_[2]) << 8) | rx_buffer_[3];
        } else if (payload_len_7 == 127) {
            header_size += 8;
            if (rx_buffer_.size() < header_size) return; // Need more data
            uint64_t big_len = 0;
            for (size_t i = 0; i < 8; ++i) {
                big_len = (big_len << 8) | rx_buffer_[2 + i];
            }
            if (big_len > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                last_error_ = "Incoming WebSocket frame is too large";
                connected_ = false;
                return;
            }
            payload_len = static_cast<size_t>(big_len);
        }
        
        if (masked) {
            header_size += 4;
        }
        
        if (rx_buffer_.size() < header_size + payload_len) {
            return; // Need more data for full frame payload
        }
        
        // We have a full frame! Extract it
        uint8_t mask_key[4] = {0};
        if (masked) {
            memcpy(mask_key, &rx_buffer_[header_size - 4], 4);
        }
        
        std::vector<uint8_t> payload(&rx_buffer_[header_size], &rx_buffer_[header_size + payload_len]);
        
        if (masked) {
            for (size_t i = 0; i < payload_len; i++) {
                payload[i] ^= mask_key[i % 4];
            }
        }
        
        // Remove parsed frame from buffer
        rx_buffer_.erase(rx_buffer_.begin(), rx_buffer_.begin() + header_size + payload_len);
        
        // Handle frame
        if (opcode == 0x01) { // Text frame
            std::string msg((char*)payload.data(), payload.size());
            if (on_message_) on_message_(msg);
        } else if (opcode == 0x08) { // Close frame
            uint16_t close_code = 0;
            if (payload.size() >= 2)
                close_code = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
            std::string close_reason;
            if (payload.size() > 2)
                close_reason.assign(payload.begin() + 2, payload.end());
            last_error_ = "Signaling closed";
            if (close_code != 0)
                last_error_ += " code=" + std::to_string(close_code);
            if (!close_reason.empty())
                last_error_ += " reason=" + close_reason;
            send_frame(0x88, payload.data(), payload.size());
            connected_ = false;
        } else if (opcode == 0x09) { // Ping frame
            send_frame(0x8A, payload.data(), payload.size()); // Send Pong
        }
    }
}
