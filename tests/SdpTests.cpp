#include "opennow/gfn/Sdp.hpp"
#include "opennow/gfn/SignalingClient.hpp"

#include <cassert>
#include <string>

int main() {
    const opennow::gfn::SignalingClient signaling({
        .signalingServer = "80-250-97-37.cloudmatchbeta.nvidiagrid.net",
        .sessionId = "session-123",
        .signalingUrl = "wss://80-250-97-37.cloudmatchbeta.nvidiagrid.net/nvst/",
    });
    assert(signaling.signInUrl().find("/nvst/sign_in?peer_id=peer-") != std::string::npos);
    assert(signaling.signInUrl().find("&peer_role=1&pairing_id=session-123") != std::string::npos);
    assert(signaling.websocketProtocol() == "x-nv-sessionid.session-123");
    assert(signaling.peerInfoJson(0, "1280x720").find("\"browser\":\"Chrome\"") != std::string::npos);

    assert(opennow::gfn::extractPublicIp("80-250-97-40.cloudmatchbeta.nvidiagrid.net") == "80.250.97.40");
    assert(opennow::gfn::extractPublicIp("203.0.113.5") == "203.0.113.5");
    assert(opennow::gfn::extractPublicIp("np-ams-01.cloudmatchbeta.nvidiagrid.net").empty());

    const std::string offer =
        "v=0\n"
        "a=ice-ufrag:ufrag123\n"
        "a=ice-pwd:pwd456\n"
        "a=fingerprint:sha-256 AA:BB:CC\n"
        "c=IN IP4 0.0.0.0\n"
        "a=candidate:1 1 udp 2130706431 0.0.0.0 49000 typ host\n";

    const auto fixed = opennow::gfn::fixServerIp(offer, "80-250-97-40.cloudmatchbeta.nvidiagrid.net");
    assert(fixed.find("c=IN IP4 80.250.97.40") != std::string::npos);
    assert(fixed.find("2130706431 80.250.97.40 49000") != std::string::npos);

    assert(opennow::gfn::extractIceUfragFromOffer(offer) == "ufrag123");
    const auto credentials = opennow::gfn::extractIceCredentials(offer);
    assert(credentials.ufrag == "ufrag123");
    assert(credentials.pwd == "pwd456");
    assert(credentials.fingerprint == "AA:BB:CC");

    const std::string sessionLevelOffer =
        "v=0\r\n"
        "a=ice-ufrag:session-user\r\n"
        "a=ice-pwd:session-password\r\n"
        "a=fingerprint:sha-256 11:22\r\n"
        "a=setup:actpass\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=rtpmap:111 OPUS/48000/2\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98\r\n"
        "a=rtpmap:96 VP8/90000\r\n"
        "a=rtpmap:97 H264/90000\r\n"
        "a=rtpmap:98 rtx/90000\r\n"
        "a=fmtp:98 apt=97\r\n";
    const auto normalized = opennow::gfn::duplicateSessionWebrtcAttributesToMedia(sessionLevelOffer);
    assert(normalized.find("m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=ice-ufrag:session-user") != std::string::npos);
    assert(normalized.find("m=video 9 UDP/TLS/RTP/SAVPF 96 97 98\r\na=ice-ufrag:session-user") != std::string::npos);
    const auto summary = opennow::gfn::summarizeMediaTransportAttributes(normalized);
    assert(summary.find("mediaSections=2") != std::string::npos);
    assert(summary.find("mediaFingerprints=2") != std::string::npos);
    assert(summary.find("mediaIcePwd=2") != std::string::npos);

    const auto preferred = opennow::gfn::preferCodec(sessionLevelOffer, opennow::VideoCodec::H264);
    assert(preferred.find("m=video 9 UDP/TLS/RTP/SAVPF 97") != std::string::npos);
    assert(preferred.find("a=rtpmap:96 VP8/90000") == std::string::npos);
    assert(preferred.find("a=rtpmap:97 H264/90000") != std::string::npos);
    assert(preferred.find("a=fmtp:98 apt=97") != std::string::npos);

    const auto munged = opennow::gfn::mungeAnswerSdp(
        "m=video 9 UDP/TLS/RTP/SAVPF 96\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\n"
        "a=fmtp:111 minptime=10;useinbandfec=1\n",
        15000);
    assert(munged.find("b=AS:15000") != std::string::npos);
    assert(munged.find("b=AS:128") != std::string::npos);
    assert(munged.find("stereo=1") != std::string::npos);

    const auto nvst = opennow::gfn::buildNvstSdp({
        .width = 1280,
        .height = 720,
        .fps = 60,
        .maxBitrateKbps = 15000,
        .credentials = credentials,
    });
    assert(nvst.find("a=general.icePassword:pwd456") != std::string::npos);
    assert(nvst.find("a=video.clientViewportWd:1280") != std::string::npos);
    assert(nvst.find("a=video.clientViewportHt:720") != std::string::npos);
    assert(nvst.find("a=video.maxFPS:60") != std::string::npos);
    assert(nvst.find("m=application 0 RTP/AVP") != std::string::npos);

    const std::string answer =
        "v=0\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 97\n"
        "a=ice-ufrag:local-user\n"
        "a=ice-pwd:local-password\n"
        "a=fingerprint:sha-256 DD:EE:FF\n"
        "a=rtpmap:97 H264/90000\n";
    const auto answerNvst = opennow::gfn::buildNvstSdpForAnswer({
        .width = 1280,
        .height = 720,
        .fps = 60,
        .maxBitrateKbps = 15000,
        .credentials = credentials,
    }, answer);
    assert(answerNvst.find("a=general.icePassword:local-password") != std::string::npos);
    assert(answerNvst.find("a=general.iceUserNameFragment:local-user") != std::string::npos);

    return 0;
}
