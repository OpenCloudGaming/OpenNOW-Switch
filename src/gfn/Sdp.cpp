#include "opennow/gfn/Sdp.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace opennow::gfn {

namespace {

std::vector<std::string> splitLines(const std::string& value) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty() || (!value.empty() && value.back() == '\n')) {
        lines.push_back(current);
    }
    return lines;
}

std::vector<std::string> splitNonEmptyLines(const std::string& value) {
    std::vector<std::string> lines;
    for (auto& line : splitLines(value)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

const char* lineEnding(const std::string& sdp) {
    return sdp.find("\r\n") != std::string::npos ? "\r\n" : "\n";
}

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool isMediaTransportAttribute(const std::string& line) {
    return startsWith(line, "a=ice-ufrag:")
        || startsWith(line, "a=ice-pwd:")
        || startsWith(line, "a=fingerprint:")
        || startsWith(line, "a=setup:");
}

std::string replaceAll(std::string value, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
    return value;
}

std::string codecName(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264:
        return "H264";
    }
    return "H264";
}

std::string normalizeCodecName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (name == "HEVC") {
        return "H265";
    }
    return name;
}

std::string joinLines(const std::vector<std::string>& lines, const char* ending) {
    std::ostringstream out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) {
            out << ending;
        }
    }
    return out.str();
}

} // namespace

std::string extractPublicIp(const std::string& hostOrIp) {
    static const std::regex dottedIp(R"(^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$)");
    if (std::regex_match(hostOrIp, dottedIp)) {
        return hostOrIp;
    }

    const auto dot = hostOrIp.find('.');
    const auto firstLabel = dot == std::string::npos ? hostOrIp : hostOrIp.substr(0, dot);
    static const std::regex dashedIp(R"(^(\d{1,3})-(\d{1,3})-(\d{1,3})-(\d{1,3})$)");
    std::smatch match;
    if (std::regex_match(firstLabel, match, dashedIp)) {
        return match[1].str() + "." + match[2].str() + "." + match[3].str() + "." + match[4].str();
    }

    return "";
}

std::string fixServerIp(const std::string& sdp, const std::string& serverIp) {
    const auto ip = extractPublicIp(serverIp);
    if (ip.empty()) {
        return sdp;
    }

    auto fixed = replaceAll(sdp, "c=IN IP4 0.0.0.0", "c=IN IP4 " + ip);
    fixed = replaceAll(fixed, " 0.0.0.0 ", " " + ip + " ");
    return fixed;
}

std::string duplicateSessionWebrtcAttributesToMedia(const std::string& sdp) {
    const auto lines = splitNonEmptyLines(sdp);
    const auto firstMedia = std::find_if(lines.begin(), lines.end(), [](const std::string& line) {
        return startsWith(line, "m=");
    });
    if (firstMedia == lines.end()) {
        return sdp;
    }

    std::vector<std::string> sessionAttributes;
    for (auto it = lines.begin(); it != firstMedia; ++it) {
        if (startsWith(*it, "a=ice-ufrag:")
            || startsWith(*it, "a=ice-pwd:")
            || startsWith(*it, "a=ice-options:")
            || startsWith(*it, "a=fingerprint:")
            || startsWith(*it, "a=setup:")) {
            sessionAttributes.push_back(*it);
        }
    }
    if (sessionAttributes.empty()) {
        return sdp;
    }

    std::vector<std::string> output;
    for (auto it = lines.begin(); it != firstMedia; ++it) {
        if (!isMediaTransportAttribute(*it)) {
            output.push_back(*it);
        }
    }

    auto index = static_cast<std::size_t>(std::distance(lines.begin(), firstMedia));
    while (index < lines.size()) {
        const auto sectionStart = index++;
        while (index < lines.size() && !startsWith(lines[index], "m=")) {
            ++index;
        }

        const auto sectionEnd = index;
        auto insertIndex = sectionEnd;
        for (auto i = sectionStart; i < sectionEnd; ++i) {
            if (startsWith(lines[i], "a=")) {
                insertIndex = i;
                break;
            }
        }

        for (auto i = sectionStart; i < insertIndex; ++i) {
            output.push_back(lines[i]);
        }

        for (const auto& attribute : sessionAttributes) {
            const auto colon = attribute.find(':');
            const auto prefix = colon == std::string::npos ? attribute : attribute.substr(0, colon + 1);
            bool alreadyPresent = false;
            for (auto i = sectionStart; i < sectionEnd; ++i) {
                if (startsWith(lines[i], prefix.c_str())) {
                    alreadyPresent = true;
                    break;
                }
            }
            if (!alreadyPresent) {
                output.push_back(attribute);
            }
        }

        for (auto i = insertIndex; i < sectionEnd; ++i) {
            output.push_back(lines[i]);
        }
    }

    return joinLines(output, lineEnding(sdp));
}

std::string summarizeMediaTransportAttributes(const std::string& sdp) {
    const auto lines = splitNonEmptyLines(sdp);
    bool sessionFingerprint = false;
    for (const auto& line : lines) {
        if (startsWith(line, "m=")) {
            break;
        }
        if (startsWith(line, "a=fingerprint:")) {
            sessionFingerprint = true;
            break;
        }
    }

    std::size_t mediaCount = 0;
    std::size_t fingerprintCount = 0;
    std::size_t setupCount = 0;
    std::size_t iceUfragCount = 0;
    std::size_t icePwdCount = 0;

    for (std::size_t i = 0; i < lines.size();) {
        if (!startsWith(lines[i], "m=")) {
            ++i;
            continue;
        }
        ++mediaCount;
        ++i;
        bool hasFingerprint = false;
        bool hasSetup = false;
        bool hasIceUfrag = false;
        bool hasIcePwd = false;
        while (i < lines.size() && !startsWith(lines[i], "m=")) {
            hasFingerprint = hasFingerprint || startsWith(lines[i], "a=fingerprint:");
            hasSetup = hasSetup || startsWith(lines[i], "a=setup:");
            hasIceUfrag = hasIceUfrag || startsWith(lines[i], "a=ice-ufrag:");
            hasIcePwd = hasIcePwd || startsWith(lines[i], "a=ice-pwd:");
            ++i;
        }
        fingerprintCount += hasFingerprint ? 1 : 0;
        setupCount += hasSetup ? 1 : 0;
        iceUfragCount += hasIceUfrag ? 1 : 0;
        icePwdCount += hasIcePwd ? 1 : 0;
    }

    std::ostringstream summary;
    summary
        << "mediaSections=" << mediaCount
        << ", mediaFingerprints=" << fingerprintCount
        << ", mediaSetup=" << setupCount
        << ", mediaIceUfrag=" << iceUfragCount
        << ", mediaIcePwd=" << icePwdCount
        << ", sessionFingerprint=" << (sessionFingerprint ? "true" : "false");
    return summary.str();
}

std::string extractIceUfragFromOffer(const std::string& sdp) {
    static const std::regex pattern(R"(a=ice-ufrag:([^\r\n]+))");
    std::smatch match;
    if (std::regex_search(sdp, match, pattern)) {
        return match[1].str();
    }
    return "";
}

IceCredentials extractIceCredentials(const std::string& sdp) {
    IceCredentials credentials;
    for (const auto& line : splitLines(sdp)) {
        if (line.rfind("a=ice-ufrag:", 0) == 0) {
            credentials.ufrag = line.substr(12);
        } else if (line.rfind("a=ice-pwd:", 0) == 0) {
            credentials.pwd = line.substr(10);
        } else if (line.rfind("a=fingerprint:sha-256 ", 0) == 0) {
            credentials.fingerprint = line.substr(22);
        }
    }
    return credentials;
}

std::optional<VideoCodec> extractNegotiatedVideoCodec(const std::string& sdp) {
    const auto lines = splitNonEmptyLines(sdp);
    bool inVideo = false;
    std::vector<std::string> videoPayloads;
    std::unordered_map<std::string, std::string> codecByPayload;

    for (const auto& line : lines) {
        if (startsWith(line, "m=video")) {
            inVideo = true;
            std::istringstream parts(line);
            std::string token;
            for (int i = 0; i < 3 && parts >> token; ++i) {}
            while (parts >> token) {
                videoPayloads.push_back(token);
            }
            continue;
        }
        if (startsWith(line, "m=") && inVideo) {
            inVideo = false;
        }
        if (!inVideo || !startsWith(line, "a=rtpmap:")) {
            continue;
        }
        const auto rest = line.substr(9);
        std::istringstream parts(rest);
        std::string payload;
        std::string codecPart;
        if (!(parts >> payload >> codecPart)) {
            continue;
        }
        const auto slash = codecPart.find('/');
        codecByPayload[payload] = normalizeCodecName(slash == std::string::npos ? codecPart : codecPart.substr(0, slash));
    }

    for (const auto& payload : videoPayloads) {
        const auto found = codecByPayload.find(payload);
        if (found == codecByPayload.end()) {
            continue;
        }
        if (found->second == "H264") {
            return VideoCodec::H264;
        }
    }
    return std::nullopt;
}

std::string preferCodec(const std::string& sdp, VideoCodec codec) {
    const auto target = codecName(codec);
    const auto lines = splitNonEmptyLines(sdp);
    bool inVideo = false;
    std::unordered_map<std::string, std::string> codecByPayload;
    std::unordered_map<std::string, std::string> rtxAptByPayload;
    std::vector<std::string> preferredPayloads;

    for (const auto& line : lines) {
        if (startsWith(line, "m=video")) {
            inVideo = true;
            continue;
        }
        if (startsWith(line, "m=") && inVideo) {
            inVideo = false;
        }
        if (!inVideo || !startsWith(line, "a=rtpmap:")) {
            continue;
        }

        std::istringstream parts(line.substr(9));
        std::string payload;
        std::string codecPart;
        if (!(parts >> payload >> codecPart)) {
            continue;
        }
        const auto slash = codecPart.find('/');
        const auto name = normalizeCodecName(slash == std::string::npos ? codecPart : codecPart.substr(0, slash));
        codecByPayload[payload] = name;
        if (name == target) {
            preferredPayloads.push_back(payload);
        }
    }

    if (preferredPayloads.empty()) {
        return sdp;
    }

    inVideo = false;
    static const std::regex aptPattern(R"((?:^|[;\s])apt=(\d+))", std::regex::icase);
    for (const auto& line : lines) {
        if (startsWith(line, "m=video")) {
            inVideo = true;
            continue;
        }
        if (startsWith(line, "m=") && inVideo) {
            inVideo = false;
        }
        if (!inVideo || !startsWith(line, "a=fmtp:")) {
            continue;
        }
        std::istringstream parts(line.substr(7));
        std::string payload;
        if (!(parts >> payload)) {
            continue;
        }
        std::smatch match;
        if (std::regex_search(line, match, aptPattern)) {
            rtxAptByPayload[payload] = match[1].str();
        }
    }

    std::unordered_set<std::string> preferred(preferredPayloads.begin(), preferredPayloads.end());
    std::unordered_set<std::string> allowed = preferred;
    for (const auto& [rtxPayload, apt] : rtxAptByPayload) {
        const auto codecFound = codecByPayload.find(rtxPayload);
        if (preferred.contains(apt) && codecFound != codecByPayload.end() && codecFound->second == "RTX") {
            allowed.insert(rtxPayload);
        }
    }

    std::vector<std::string> output;
    inVideo = false;
    for (const auto& line : lines) {
        if (startsWith(line, "m=video")) {
            inVideo = true;
            std::istringstream parts(line);
            std::vector<std::string> tokens;
            std::string token;
            while (parts >> token) {
                tokens.push_back(token);
            }
            if (tokens.size() <= 3) {
                output.push_back(line);
                continue;
            }
            std::vector<std::string> mediaLine{tokens[0], tokens[1], tokens[2]};
            for (const auto& payload : preferredPayloads) {
                if (std::find(tokens.begin() + 3, tokens.end(), payload) != tokens.end()) {
                    mediaLine.push_back(payload);
                }
            }
            for (auto it = tokens.begin() + 3; it != tokens.end(); ++it) {
                if (allowed.contains(*it) && !preferred.contains(*it)) {
                    mediaLine.push_back(*it);
                }
            }
            output.push_back(joinLines(mediaLine, " "));
            continue;
        }

        if (startsWith(line, "m=") && inVideo) {
            inVideo = false;
        }

        if (inVideo && (startsWith(line, "a=rtpmap:") || startsWith(line, "a=fmtp:") || startsWith(line, "a=rtcp-fb:"))) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::istringstream parts(line.substr(colon + 1));
                std::string payload;
                parts >> payload;
                if (!payload.empty() && !allowed.contains(payload)) {
                    continue;
                }
            }
        }

        output.push_back(line);
    }

    return joinLines(output, lineEnding(sdp));
}

std::string mungeAnswerSdp(const std::string& sdp, std::uint32_t maxBitrateKbps) {
    const auto lineEnding = sdp.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::ostringstream out;
    const auto lines = splitLines(sdp);

    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto line = lines[i];
        if (line.rfind("a=fmtp:", 0) == 0 && line.find("minptime=") != std::string::npos && line.find("stereo=1") == std::string::npos) {
            line += ";stereo=1";
        }

        out << line;
        if (i + 1 < lines.size()) {
            out << lineEnding;
        }

        if (line.rfind("m=video", 0) == 0 || line.rfind("m=audio", 0) == 0) {
            const auto next = i + 1 < lines.size() ? lines[i + 1] : "";
            if (next.rfind("b=", 0) != 0) {
                out << lineEnding << "b=AS:" << (line.rfind("m=video", 0) == 0 ? maxBitrateKbps : 128);
            }
        }
    }
    return out.str();
}

std::string buildNvstSdp(const NvstSdpParams& params) {
    const auto minBitrate = params.maxBitrateKbps * 35 / 100 > 5000
        ? params.maxBitrateKbps * 35 / 100
        : 5000;
    const auto initialBitrate = params.maxBitrateKbps * 70 / 100 > minBitrate
        ? params.maxBitrateKbps * 70 / 100
        : minBitrate;

    std::ostringstream sdp;
    sdp
        << "v=0\n"
        << "o=SdpTest test_id_13 14 IN IPv4 127.0.0.1\n"
        << "s=-\n"
        << "t=0 0\n"
        << "a=general.icePassword:" << params.credentials.pwd << "\n"
        << "a=general.iceUserNameFragment:" << params.credentials.ufrag << "\n"
        << "a=general.dtlsFingerprint:" << params.credentials.fingerprint << "\n"
        << "m=video 0 RTP/AVP\n"
        << "a=msid:fbc-video-0\n"
        << "a=vqos.fec.rateDropWindow:10\n"
        << "a=vqos.fec.minRequiredFecPackets:2\n"
        << "a=vqos.fec.repairMinPercent:5\n"
        << "a=vqos.fec.repairPercent:5\n"
        << "a=vqos.fec.repairMaxPercent:35\n"
        << "a=vqos.drc.enable:0\n"
        << "a=vqos.dfc.enable:0\n"
        << "a=video.dx9EnableNv12:1\n"
        << "a=video.enableRtpNack:1\n"
        << "a=video.packetSize:1140\n"
        << "a=video.clientViewportWd:" << params.width << "\n"
        << "a=video.clientViewportHt:" << params.height << "\n"
        << "a=video.maxFPS:" << params.fps << "\n"
        << "a=video.initialBitrateKbps:" << initialBitrate << "\n"
        << "a=video.initialPeakBitrateKbps:" << params.maxBitrateKbps << "\n"
        << "a=vqos.bw.maximumBitrateKbps:" << params.maxBitrateKbps << "\n"
        << "a=vqos.bw.minimumBitrateKbps:" << minBitrate << "\n"
        << "a=vqos.bw.peakBitrateKbps:" << params.maxBitrateKbps << "\n"
        << "a=vqos.bw.serverPeakBitrateKbps:" << params.maxBitrateKbps << "\n"
        << "a=video.codec:" << codecName(params.codec) << "\n"
        << "a=video.bitDepth:8\n"
        << "a=video.scalingFeature1:0\n"
        << "a=video.prefilterParams.prefilterModel:0\n"
        << "m=audio 0 RTP/AVP\n"
        << "a=msid:audio\n"
        << "m=mic 0 RTP/AVP\n"
        << "a=msid:mic\n"
        << "a=rtpmap:0 PCMU/8000\n"
        << "m=application 0 RTP/AVP\n"
        << "a=msid:input_1\n"
        << "a=ri.partialReliableThresholdMs:" << params.partialReliableThresholdMs << "\n"
        << "a=ri.hidDeviceMask:" << params.hidDeviceMask << "\n"
        << "a=ri.enablePartiallyReliableTransferGamepad:" << params.enablePartiallyReliableTransferGamepad << "\n"
        << "a=ri.enablePartiallyReliableTransferHid:" << params.enablePartiallyReliableTransferHid << "\n";

    return sdp.str();
}

std::string buildNvstSdpForAnswer(const NvstSdpParams& params, const std::string& answerSdp) {
    auto answerParams = params;
    answerParams.credentials = extractIceCredentials(answerSdp);
    if (answerParams.credentials.ufrag.empty()
        || answerParams.credentials.pwd.empty()
        || answerParams.credentials.fingerprint.empty()) {
        return "";
    }
    if (const auto codec = extractNegotiatedVideoCodec(answerSdp)) {
        answerParams.codec = *codec;
    }
    return buildNvstSdp(answerParams);
}

} // namespace opennow::gfn
