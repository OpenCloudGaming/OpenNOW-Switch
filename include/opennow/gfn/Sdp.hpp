#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "opennow/core/Settings.hpp"

namespace opennow::gfn {

struct IceCredentials {
    std::string ufrag;
    std::string pwd;
    std::string fingerprint;
};

struct NvstSdpParams {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t fps = 60;
    std::uint32_t maxBitrateKbps = 15000;
    std::uint32_t partialReliableThresholdMs = 300;
    std::uint32_t hidDeviceMask = 0xffffffffu;
    std::uint32_t enablePartiallyReliableTransferGamepad = 0x0f;
    std::uint32_t enablePartiallyReliableTransferHid = 0xffffffffu;
    VideoCodec codec = VideoCodec::H264;
    IceCredentials credentials;
};

std::string extractPublicIp(const std::string& hostOrIp);
std::string fixServerIp(const std::string& sdp, const std::string& serverIp);
std::string duplicateSessionWebrtcAttributesToMedia(const std::string& sdp);
std::string summarizeMediaTransportAttributes(const std::string& sdp);
std::string extractIceUfragFromOffer(const std::string& sdp);
IceCredentials extractIceCredentials(const std::string& sdp);
std::optional<VideoCodec> extractNegotiatedVideoCodec(const std::string& sdp);
std::string preferCodec(const std::string& sdp, VideoCodec codec);
std::string mungeAnswerSdp(const std::string& sdp, std::uint32_t maxBitrateKbps);
std::string buildNvstSdp(const NvstSdpParams& params);
std::string buildNvstSdpForAnswer(const NvstSdpParams& params, const std::string& answerSdp);

} // namespace opennow::gfn
