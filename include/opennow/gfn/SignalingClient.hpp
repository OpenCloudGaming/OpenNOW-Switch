#pragma once

#include <cstdint>
#include <string>

namespace opennow::gfn {

struct SignalingEndpoint {
    std::string signalingServer;
    std::string sessionId;
    std::string signalingUrl;
};

class SignalingClient {
public:
    explicit SignalingClient(SignalingEndpoint endpoint);

    std::string signInUrl() const;
    std::string websocketProtocol() const;
    std::string peerInfoJson(std::uint32_t peerId, const std::string& resolution = "1280x720") const;
    const std::string& peerName() const;

private:
    SignalingEndpoint endpoint_;
    std::string peerName_;
};

} // namespace opennow::gfn
