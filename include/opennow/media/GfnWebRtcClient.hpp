#pragma once

#include "opennow/gfn/GfnTypes.hpp"
#include "opennow/gfn/InputEncoder.hpp"
#include "opennow/core/Settings.hpp"
#include "opennow/net/HttpClient.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace opennow::media {

struct GfnWebRtcConfig {
    SessionInfo session;
};

struct GfnWebRtcStartResult {
    bool ok = false;
    std::string answerSdp;
    std::string error;
};

struct GfnWebRtcSignalingConfig {
    SessionInfo session;
    StreamSettings settings;
    net::HttpClient* http = nullptr;
    std::function<void(const std::string&)> log;
    std::function<void(const std::uint8_t*, std::size_t)> videoNal;
    std::function<void(const std::uint8_t*, std::size_t)> opusPacket;
    std::uint32_t timeoutMs = 30000;
};

struct GfnWebRtcSignalingResult {
    bool ok = false;
    std::uint32_t localPeerId = 0;
    std::string remotePeerId;
    std::string offerSdp;
    std::string answerSdp;
    std::string nvstSdp;
    std::string error;
};

class GfnWebRtcClient {
public:
    GfnWebRtcClient();
    ~GfnWebRtcClient();

    GfnWebRtcClient(const GfnWebRtcClient&) = delete;
    GfnWebRtcClient& operator=(const GfnWebRtcClient&) = delete;

    GfnWebRtcStartResult startFromRemoteOffer(const GfnWebRtcConfig& config, const std::string& offerSdp);
    GfnWebRtcSignalingResult connectWithGfnSignaling(const GfnWebRtcSignalingConfig& config);
    bool addRemoteCandidate(const std::string& candidate);
    bool pumpOnce();
    bool openInputChannels(std::uint32_t partialReliableThresholdMs = 16);
    bool inputReady() const;
    bool sendReliableInput(const gfn::Bytes& payload);
    bool sendPartiallyReliableInput(const gfn::Bytes& payload);
    bool sendGamepadInput(const gfn::GamepadInput& input, std::uint16_t connectedBitmap = 0x0101);
    bool sendMouseMove(const gfn::MouseMovePayload& input);
    bool sendMouseButton(const gfn::MouseButtonPayload& input, bool pressed);
    std::string connectionState() const;
    bool mediaConnected() const;
    void close();

    std::size_t videoBytesReceived() const;
    std::size_t audioBytesReceived() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace opennow::media
