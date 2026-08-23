#include "webrtc/nvst_sdp.hpp"

#include <cassert>
#include <string>

namespace
{

bool HasLine(const std::string& sdp, const std::string& line)
{
    return sdp.find(line + "\n") != std::string::npos;
}

bool HasAttribute(const std::string& sdp, const std::string& attribute)
{
    return sdp.find("a=" + attribute) != std::string::npos;
}

} // namespace

int main()
{
    opennow::StreamSettings settings;
    settings.width = 1280;
    settings.height = 720;
    settings.fps = 60;
    settings.bitrate_kbps = 12000;
    settings.image_quality_mode = "Adaptive";

    const std::string answer =
        "v=0\r\n"
        "a=ice-ufrag:test-ufrag\r\n"
        "a=ice-pwd:test-password\r\n"
        "a=fingerprint:sha-256 AA:BB:CC\r\n";
    const std::string sdp = opennow::webrtc::BuildNvstSdp(
        answer, settings, opennow::webrtc::RiInputCapabilities {});

    for (const auto& line : {
        "a=video.maxFPS:60",
        "a=video.initialBitrateKbps:4000",
        "a=video.initialPeakBitrateKbps:4000",
        "a=vqos.bw.maximumBitrateKbps:12000",
        "a=vqos.bw.minimumBitrateKbps:4000",
        "a=vqos.dynamicStreamingMode:3",
        "a=vqos.drc.enable:1",
        "a=vqos.resControl.cpmRtc.featureMask:3",
    }) {
        assert(HasLine(sdp, line));
    }

    for (const auto& attribute : {
        "vqos.bw.peakBitrateKbps:",
        "vqos.bw.serverPeakBitrateKbps:",
        "vqos.bw.enableBandwidthEstimation:",
        "vqos.bw.disableBitrateLimit:",
        "vqos.grc.maximumBitrateKbps:",
        "vqos.grc.enable:",
        "vqos.dfc.enable:",
        "vqos.dfc.adjustResAndFps:",
        "vqos.resControl.cpmRtc.enable:",
        "vqos.resControl.cpmRtc.minResolutionPercent:",
        "vqos.resControl.cpmRtc.resolutionChangeHoldonMs:",
    }) {
        assert(!HasAttribute(sdp, attribute));
    }

    settings.bitrate_kbps = 20000;
    const std::string quality_sdp = opennow::webrtc::BuildNvstSdp(
        answer, settings, opennow::webrtc::RiInputCapabilities {});
    assert(HasLine(quality_sdp, "a=video.initialBitrateKbps:5000"));
    assert(HasLine(quality_sdp, "a=vqos.bw.maximumBitrateKbps:20000"));
    return 0;
}
