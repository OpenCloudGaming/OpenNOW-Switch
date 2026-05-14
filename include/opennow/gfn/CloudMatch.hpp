#pragma once

#include <string>

#include "opennow/core/Settings.hpp"
#include "opennow/gfn/GfnTypes.hpp"
#include "opennow/net/HttpClient.hpp"
#include "opennow/util/Json.hpp"

namespace opennow::gfn {

struct SessionCreateRequest {
    std::string token;
    std::string streamingBaseUrl;
    std::string zone;
    std::string appId;
    std::string internalTitle;
    std::string clientId;
    std::string deviceId;
    StreamSettings settings;
    bool accountLinked = true;
};

struct SessionPollRequest {
    std::string token;
    std::string streamingBaseUrl;
    std::string serverIp;
    std::string zone;
    std::string sessionId;
    std::string clientId;
    std::string deviceId;
};

struct SessionStopRequest {
    std::string token;
    std::string streamingBaseUrl;
    std::string serverIp;
    std::string zone;
    std::string sessionId;
    std::string clientId;
    std::string deviceId;
};

struct SessionClaimRequest {
    std::string sessionId;
    std::string appId;
    std::string deviceId;
    std::string subSessionId;
};

class CloudMatchRequestBuilder {
public:
    std::string buildSessionCreateBody(const SessionCreateRequest& request) const;
    std::string buildSessionClaimBody(const SessionClaimRequest& request) const;

    net::Headers buildRequestHeaders(const std::string& token, const std::string& clientId, const std::string& deviceId, bool includeOrigin) const;
    net::HttpRequest buildCreateSessionRequest(const SessionCreateRequest& request) const;
    net::HttpRequest buildPollSessionRequest(const SessionPollRequest& request) const;
    net::HttpRequest buildStopSessionRequest(const SessionStopRequest& request) const;

    std::string resolveStreamingBaseUrl(const std::string& zone, const std::string& provided) const;
    std::string resolvePollStopBase(const std::string& zone, const std::string& provided, const std::string& serverIp) const;
    bool isZoneHostname(const std::string& host) const;
};

struct CloudMatchParseResult {
    bool ok = false;
    SessionInfo session;
    std::string error;
};

class CloudMatchResponseParser {
public:
    CloudMatchParseResult parseSessionInfo(const std::string& json, const std::string& zone, const std::string& streamingBaseUrl) const;

private:
    std::string streamingServerIp(const util::JsonValue& session) const;
    std::string extractHostFromUrl(const std::string& url) const;
    std::string buildSignalingUrl(const std::string& resourcePath, const std::string& serverIp) const;
    MediaConnectionInfo resolveMediaConnectionInfo(const util::JsonValue& session, const std::string& serverIp) const;
    std::vector<IceServer> normalizeIceServers(const util::JsonValue& root) const;
};

} // namespace opennow::gfn
