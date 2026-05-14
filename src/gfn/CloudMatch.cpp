#include "opennow/gfn/CloudMatch.hpp"

#include <cstdint>
#include <sstream>
#include <utility>

namespace opennow::gfn {

namespace {

std::string escapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

std::string trimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

} // namespace

std::string CloudMatchRequestBuilder::buildSessionCreateBody(const SessionCreateRequest& request) const {
    const auto& settings = request.settings;
    std::ostringstream body;

    body
        << "{"
        << "\"sessionRequestData\":{"
        << "\"appId\":\"" << escapeJson(request.appId) << "\","
        << "\"internalTitle\":";

    if (request.internalTitle.empty()) {
        body << "null";
    } else {
        body << "\"" << escapeJson(request.internalTitle) << "\"";
    }

    body
        << ",\"availableSupportedControllers\":[]"
        << ",\"networkTestSessionId\":null"
        << ",\"parentSessionId\":null"
        << ",\"clientIdentification\":\"GFN-PC\""
        << ",\"deviceHashId\":\"" << escapeJson(request.deviceId) << "\""
        << ",\"clientVersion\":\"30.0\""
        << ",\"sdkVersion\":\"1.0\""
        << ",\"streamerVersion\":1"
        << ",\"clientPlatformName\":\"windows\""
        << ",\"clientRequestMonitorSettings\":[{"
        << "\"widthInPixels\":" << settings.width << ","
        << "\"heightInPixels\":" << settings.height << ","
        << "\"framesPerSecond\":" << settings.fps << ","
        << "\"sdrHdrMode\":0,"
        << "\"displayData\":{"
        << "\"desiredContentMaxLuminance\":0,"
        << "\"desiredContentMinLuminance\":0,"
        << "\"desiredContentMaxFrameAverageLuminance\":0"
        << "},"
        << "\"dpi\":100"
        << "}]"
        << ",\"useOps\":true"
        << ",\"audioMode\":" << (settings.stereoAudio ? 2 : 0)
        << ",\"metaData\":["
        << "{\"key\":\"wssignaling\",\"value\":\"1\"},"
        << "{\"key\":\"GSStreamerType\",\"value\":\"WebRTC\"},"
        << "{\"key\":\"networkType\",\"value\":\"Unknown\"},"
        << "{\"key\":\"ClientImeSupport\",\"value\":\"0\"},"
        << "{\"key\":\"clientPhysicalResolution\",\"value\":\"{\\\"horizontalPixels\\\":"
        << settings.width << ",\\\"verticalPixels\\\":" << settings.height << "}\"},"
        << "{\"key\":\"surroundAudioInfo\",\"value\":\"2\"}"
        << "]"
        << ",\"sdrHdrMode\":0"
        << ",\"clientDisplayHdrCapabilities\":null"
        << ",\"surroundAudioInfo\":0"
        << ",\"remoteControllersBitmap\":0"
        << ",\"clientTimezoneOffset\":0"
        << ",\"enhancedStreamMode\":1"
        << ",\"appLaunchMode\":1"
        << ",\"secureRTSPSupported\":false"
        << ",\"partnerCustomData\":\"\""
        << ",\"accountLinked\":" << (request.accountLinked ? "true" : "false")
        << ",\"enablePersistingInGameSettings\":true"
        << ",\"userAge\":26"
        << ",\"requestedStreamingFeatures\":{"
        << "\"reflex\":false,"
        << "\"bitDepth\":0,"
        << "\"cloudGsync\":false,"
        << "\"enabledL4S\":false,"
        << "\"mouseMovementFlags\":0,"
        << "\"trueHdr\":false,"
        << "\"supportedHidDevices\":0,"
        << "\"profile\":0,"
        << "\"fallbackToLogicalResolution\":false,"
        << "\"chromaFormat\":0,"
        << "\"prefilterMode\":0,"
        << "\"hudStreamingMode\":0"
        << "}"
        << "}"
        << "}";

    return body.str();
}

std::string CloudMatchRequestBuilder::buildSessionClaimBody(const SessionClaimRequest& request) const {
    const auto numericAppId = request.appId.empty() ? "0" : request.appId;
    std::ostringstream body;
    body
        << "{"
        << "\"action\":2,"
        << "\"data\":\"RESUME\","
        << "\"sessionRequestData\":{"
        << "\"audioMode\":2,"
        << "\"remoteControllersBitmap\":0,"
        << "\"sdrHdrMode\":0,"
        << "\"networkTestSessionId\":null,"
        << "\"availableSupportedControllers\":[],"
        << "\"clientVersion\":\"30.0\","
        << "\"deviceHashId\":\"" << escapeJson(request.deviceId) << "\","
        << "\"internalTitle\":null,"
        << "\"clientPlatformName\":\"windows\","
        << "\"metaData\":["
        << "{\"key\":\"SubSessionId\",\"value\":\"" << escapeJson(request.subSessionId) << "\"},"
        << "{\"key\":\"wssignaling\",\"value\":\"1\"},"
        << "{\"key\":\"GSStreamerType\",\"value\":\"WebRTC\"},"
        << "{\"key\":\"networkType\",\"value\":\"Unknown\"},"
        << "{\"key\":\"ClientImeSupport\",\"value\":\"0\"}"
        << "],"
        << "\"surroundAudioInfo\":0,"
        << "\"clientTimezoneOffset\":0,"
        << "\"clientIdentification\":\"GFN-PC\","
        << "\"parentSessionId\":null,"
        << "\"appId\":" << numericAppId << ","
        << "\"streamerVersion\":1,"
        << "\"appLaunchMode\":1,"
        << "\"sdkVersion\":\"1.0\","
        << "\"enhancedStreamMode\":1,"
        << "\"useOps\":true,"
        << "\"clientDisplayHdrCapabilities\":null,"
        << "\"accountLinked\":true,"
        << "\"partnerCustomData\":\"\","
        << "\"enablePersistingInGameSettings\":true,"
        << "\"secureRTSPSupported\":false,"
        << "\"userAge\":26,"
        << "\"requestedStreamingFeatures\":{"
        << "\"reflex\":false,"
        << "\"bitDepth\":0,"
        << "\"cloudGsync\":false,"
        << "\"profile\":0,"
        << "\"fallbackToLogicalResolution\":false,"
        << "\"chromaFormat\":0,"
        << "\"prefilterMode\":0,"
        << "\"hudStreamingMode\":0"
        << "}"
        << "},"
        << "\"metaData\":[]"
        << "}";
    return body.str();
}

net::Headers CloudMatchRequestBuilder::buildRequestHeaders(
    const std::string& token,
    const std::string& clientId,
    const std::string& deviceId,
    bool includeOrigin) const {
    net::Headers headers{
        {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36 NVIDIACEFClient/HEAD/debb5919f6 GFN-PC/2.0.80.173"},
        {"Authorization", "GFNJWT " + token},
        {"Content-Type", "application/json"},
        {"nv-browser-type", "CHROME"},
        {"nv-client-id", clientId},
        {"nv-client-streamer", "NVIDIA-CLASSIC"},
        {"nv-client-type", "NATIVE"},
        {"nv-client-version", "2.0.80.173"},
        {"nv-device-make", "UNKNOWN"},
        {"nv-device-model", "UNKNOWN"},
        {"nv-device-os", "WINDOWS"},
        {"nv-device-type", "DESKTOP"},
        {"x-device-id", deviceId},
    };

    if (includeOrigin) {
        headers["Origin"] = "https://play.geforcenow.com";
        headers["Referer"] = "https://play.geforcenow.com/";
    }

    return headers;
}

net::HttpRequest CloudMatchRequestBuilder::buildCreateSessionRequest(const SessionCreateRequest& request) const {
    const auto base = trimTrailingSlash(resolveStreamingBaseUrl(request.zone, request.streamingBaseUrl));
    return {
        net::HttpMethod::Post,
        base + "/v2/session",
        buildRequestHeaders(request.token, request.clientId, request.deviceId, true),
        buildSessionCreateBody(request),
    };
}

net::HttpRequest CloudMatchRequestBuilder::buildPollSessionRequest(const SessionPollRequest& request) const {
    const auto base = trimTrailingSlash(resolvePollStopBase(request.zone, request.streamingBaseUrl, request.serverIp));
    return {
        net::HttpMethod::Get,
        base + "/v2/session/" + request.sessionId,
        buildRequestHeaders(request.token, request.clientId, request.deviceId, false),
        "",
    };
}

net::HttpRequest CloudMatchRequestBuilder::buildStopSessionRequest(const SessionStopRequest& request) const {
    const auto base = trimTrailingSlash(resolvePollStopBase(request.zone, request.streamingBaseUrl, request.serverIp));
    return {
        net::HttpMethod::Delete,
        base + "/v2/session/" + request.sessionId,
        buildRequestHeaders(request.token, request.clientId, request.deviceId, false),
        "",
    };
}

std::string CloudMatchRequestBuilder::resolveStreamingBaseUrl(const std::string& zone, const std::string& provided) const {
    if (!provided.empty()) {
        return trimTrailingSlash(provided);
    }
    if (!zone.empty()) {
        return "https://" + zone + ".cloudmatchbeta.nvidiagrid.net";
    }
    return "https://prod.cloudmatchbeta.nvidiagrid.net";
}

std::string CloudMatchRequestBuilder::resolvePollStopBase(const std::string& zone, const std::string& provided, const std::string& serverIp) const {
    const auto base = resolveStreamingBaseUrl(zone, provided);
    if (!serverIp.empty() && !isZoneHostname(serverIp)) {
        return "https://" + serverIp;
    }
    return base;
}

bool CloudMatchRequestBuilder::isZoneHostname(const std::string& host) const {
    return host.find("cloudmatchbeta.nvidiagrid.net") != std::string::npos
        || host.find("cloudmatch.nvidiagrid.net") != std::string::npos;
}

CloudMatchParseResult CloudMatchResponseParser::parseSessionInfo(
    const std::string& json,
    const std::string& zone,
    const std::string& streamingBaseUrl) const {
    const auto parsed = util::parseJson(json);
    if (!parsed.ok) {
        return {false, {}, parsed.error};
    }

    const auto* requestStatus = parsed.value.get("requestStatus");
    const auto statusCode = requestStatus && requestStatus->get("statusCode")
        ? static_cast<int>(requestStatus->get("statusCode")->asNumber())
        : 0;
    if (statusCode != 1) {
        const auto description = requestStatus && requestStatus->get("statusDescription")
            ? requestStatus->get("statusDescription")->asString("CloudMatch request failed")
            : "CloudMatch request failed";
        return {false, {}, description};
    }

    const auto* session = parsed.value.get("session");
    if (!session || !session->isObject()) {
        return {false, {}, "CloudMatch response did not include a session object"};
    }

    SessionInfo info;
    info.zone = zone;
    info.streamingBaseUrl = streamingBaseUrl;
    if (const auto* value = session->get("sessionId")) {
        info.sessionId = value->asString();
    }
    if (const auto* value = session->get("status")) {
        info.status = static_cast<std::uint32_t>(value->asNumber());
    }
    if (const auto* value = session->get("queuePosition")) {
        info.queuePosition = static_cast<std::uint32_t>(value->asNumber());
    } else if (const auto* setup = session->get("seatSetupInfo"); setup && setup->get("queuePosition")) {
        info.queuePosition = static_cast<std::uint32_t>(setup->get("queuePosition")->asNumber());
    }
    if (const auto* value = session->get("gpuType")) {
        info.gpuType = value->asString();
    }

    info.serverIp = streamingServerIp(*session);
    if (info.serverIp.empty() && info.status != 2 && info.status != 3 && !info.sessionId.empty()) {
        return {true, info, {}};
    }
    if (info.serverIp.empty()) {
        return {false, {}, "CloudMatch response did not include a signaling host"};
    }

    std::string resourcePath = "/nvst/";
    if (const auto* connections = session->get("connectionInfo"); connections && connections->isArray()) {
        for (const auto& conn : connections->asArray()) {
            const auto usage = conn.get("usage") ? static_cast<int>(conn.get("usage")->asNumber()) : 0;
            if (usage == 14) {
                if (const auto* value = conn.get("resourcePath")) {
                    resourcePath = value->asString(resourcePath);
                }
                break;
            }
        }
    }

    info.signalingUrl = buildSignalingUrl(resourcePath, info.serverIp);
    const auto hostStart = info.signalingUrl.rfind("wss://", 0) == 0 ? 6 : 0;
    const auto hostEnd = info.signalingUrl.find('/', hostStart);
    auto host = info.signalingUrl.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
    info.signalingServer = host.find(':') == std::string::npos ? host + ":443" : host;
    info.mediaConnectionInfo = resolveMediaConnectionInfo(*session, info.serverIp);
    info.iceServers = normalizeIceServers(parsed.value);

    return {true, info, {}};
}

std::string CloudMatchResponseParser::streamingServerIp(const util::JsonValue& session) const {
    if (const auto* connections = session.get("connectionInfo"); connections && connections->isArray()) {
        for (const auto& conn : connections->asArray()) {
            const auto usage = conn.get("usage") ? static_cast<int>(conn.get("usage")->asNumber()) : 0;
            if (usage != 14) {
                continue;
            }
            if (const auto* ip = conn.get("ip"); ip && ip->isString() && !ip->asString().empty()) {
                return ip->asString();
            }
            if (const auto* resourcePath = conn.get("resourcePath"); resourcePath && resourcePath->isString()) {
                const auto host = extractHostFromUrl(resourcePath->asString());
                if (!host.empty()) {
                    return host;
                }
            }
        }
    }

    if (const auto* control = session.get("sessionControlInfo"); control && control->get("ip")) {
        return control->get("ip")->asString();
    }

    return "";
}

std::string CloudMatchResponseParser::extractHostFromUrl(const std::string& url) const {
    const char* prefixes[] = {"rtsps://", "rtsp://", "wss://", "https://"};
    std::size_t start = std::string::npos;
    for (const auto* prefix : prefixes) {
        const std::string p(prefix);
        if (url.rfind(p, 0) == 0) {
            start = p.size();
            break;
        }
    }
    if (start == std::string::npos) {
        return "";
    }
    const auto end = url.find_first_of(":/", start);
    auto host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    return host.empty() || host.front() == '.' ? "" : host;
}

std::string CloudMatchResponseParser::buildSignalingUrl(const std::string& resourcePath, const std::string& serverIp) const {
    if (resourcePath.rfind("wss://", 0) == 0) {
        return resourcePath;
    }
    if (resourcePath.rfind("rtsps://", 0) == 0 || resourcePath.rfind("rtsp://", 0) == 0) {
        const auto host = extractHostFromUrl(resourcePath);
        if (!host.empty()) {
            return "wss://" + host + "/nvst/";
        }
    }
    if (!resourcePath.empty() && resourcePath.front() == '/') {
        return "wss://" + serverIp + ":443" + resourcePath;
    }
    return "wss://" + serverIp + ":443/nvst/";
}

MediaConnectionInfo CloudMatchResponseParser::resolveMediaConnectionInfo(const util::JsonValue& session, const std::string& serverIp) const {
    const auto* connections = session.get("connectionInfo");
    if (!connections || !connections->isArray()) {
        return {};
    }

    auto tryUsage = [&](int wantedUsage, bool allowServerFallback) -> MediaConnectionInfo {
        for (const auto& conn : connections->asArray()) {
            const auto usage = conn.get("usage") ? static_cast<int>(conn.get("usage")->asNumber()) : 0;
            if (usage != wantedUsage) {
                continue;
            }
            std::string ip;
            if (const auto* value = conn.get("ip"); value && value->isString()) {
                ip = value->asString();
            }
            if (ip.empty()) {
                if (const auto* value = conn.get("resourcePath"); value && value->isString()) {
                    ip = extractHostFromUrl(value->asString());
                }
            }
            if (ip.empty() && allowServerFallback) {
                ip = serverIp;
            }
            const auto port = conn.get("port") ? static_cast<std::uint16_t>(conn.get("port")->asNumber()) : static_cast<std::uint16_t>(0);
            if (!ip.empty() && port > 0) {
                return {ip, port};
            }
        }
        return {};
    };

    if (auto media = tryUsage(2, false); !media.ip.empty()) return media;
    if (auto media = tryUsage(17, false); !media.ip.empty()) return media;
    if (auto media = tryUsage(14, true); !media.ip.empty()) return media;
    return {};
}

std::vector<IceServer> CloudMatchResponseParser::normalizeIceServers(const util::JsonValue& root) const {
    std::vector<IceServer> out;
    const auto* session = root.get("session");
    const auto* config = session ? session->get("iceServerConfiguration") : nullptr;
    const auto* servers = config ? config->get("iceServers") : nullptr;
    if (!servers || !servers->isArray()) {
        out.push_back({{"stun:s1.stun.gamestream.nvidia.com:19308"}, {}, {}});
        out.push_back({{"stun:stun.l.google.com:19302"}, {}, {}});
        return out;
    }

    for (const auto& entry : servers->asArray()) {
        IceServer server;
        if (const auto* urls = entry.get("urls")) {
            if (urls->isArray()) {
                for (const auto& url : urls->asArray()) {
                    server.urls.push_back(url.asString());
                }
            } else if (urls->isString()) {
                server.urls.push_back(urls->asString());
            }
        }
        if (const auto* username = entry.get("username")) {
            server.username = username->asString();
        }
        if (const auto* credential = entry.get("credential")) {
            server.credential = credential->asString();
        }
        if (!server.urls.empty()) {
            out.push_back(std::move(server));
        }
    }

    return out;
}

} // namespace opennow::gfn
