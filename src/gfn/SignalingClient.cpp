#include "opennow/gfn/SignalingClient.hpp"

#include <cstdlib>
#include <sstream>
#include <utility>

namespace opennow::gfn {

namespace {

std::string defaultPeerName() {
    const auto value = 1'000'000 + (std::rand() % 9'000'000);
    return "peer-" + std::to_string(value);
}

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string trimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

} // namespace

SignalingClient::SignalingClient(SignalingEndpoint endpoint)
    : endpoint_(std::move(endpoint)), peerName_(defaultPeerName()) {}

std::string SignalingClient::signInUrl() const {
    std::string base = endpoint_.signalingUrl.empty()
        ? "wss://" + endpoint_.signalingServer + "/nvst/"
        : endpoint_.signalingUrl;

    if (!startsWith(base, "wss://")) {
        base = "wss://" + base;
    }

    base = trimTrailingSlash(base);

    std::ostringstream url;
    url << base << "/sign_in"
        << "?peer_id=" << peerName_
        << "&version=2"
        << "&peer_role=1"
        << "&pairing_id=" << endpoint_.sessionId;
    return url.str();
}

std::string SignalingClient::websocketProtocol() const {
    return "x-nv-sessionid." + endpoint_.sessionId;
}

std::string SignalingClient::peerInfoJson(std::uint32_t peerId, const std::string& resolution) const {
    std::ostringstream json;
    json
        << "{\"ackid\":1,\"peer_info\":{"
        << "\"browser\":\"Chrome\","
        << "\"browserVersion\":\"131\","
        << "\"connected\":true,"
        << "\"id\":" << peerId << ","
        << "\"name\":\"" << peerName_ << "\","
        << "\"peerRole\":0,"
        << "\"resolution\":\"" << resolution << "\","
        << "\"version\":2"
        << "}}";
    return json.str();
}

const std::string& SignalingClient::peerName() const {
    return peerName_;
}

} // namespace opennow::gfn
