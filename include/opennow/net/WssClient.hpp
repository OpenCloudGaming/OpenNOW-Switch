#pragma once

#include <chrono>
#include <string>

namespace opennow::net {

struct WssConnectOptions {
    std::string url;
    std::string protocol;
    std::string origin = "https://play.geforcenow.com";
    std::string userAgent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131 Safari/537.36";
    bool verifyPeer = false;
    std::chrono::milliseconds timeout{10000};
};

struct WssResult {
    bool ok = false;
    std::string error;
};

class WssClient {
public:
    WssClient();
    ~WssClient();

    WssClient(const WssClient&) = delete;
    WssClient& operator=(const WssClient&) = delete;

    WssResult connect(const WssConnectOptions& options);
    WssResult sendText(const std::string& text);
    WssResult receiveText(std::string& text, std::chrono::milliseconds timeout);
    void close();

private:
    struct Impl;
    Impl* impl_;
};

} // namespace opennow::net
