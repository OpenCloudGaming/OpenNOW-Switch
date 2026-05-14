#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennow {

struct LoginProvider {
    std::string idpId;
    std::string code;
    std::string displayName;
    std::string streamingServiceUrl;
    std::uint32_t priority = 0;
};

struct AuthTokens {
    std::string accessToken;
    std::string refreshToken;
    std::string idToken;
    std::uint64_t expiresAtMs = 0;
    std::string clientToken;
    std::uint64_t clientTokenExpiresAtMs = 0;
};

struct AuthUser {
    std::string userId;
    std::string displayName;
    std::string email;
    std::string avatarUrl;
    std::string membershipTier = "FREE";
};

struct AuthSession {
    LoginProvider provider;
    AuthTokens tokens;
    AuthUser user;
};

struct IceServer {
    std::vector<std::string> urls;
    std::string username;
    std::string credential;
};

struct MediaConnectionInfo {
    std::string ip;
    std::uint16_t port = 0;
};

struct SessionInfo {
    std::string sessionId;
    std::string zone;
    std::string streamingBaseUrl;
    std::string signalingServer;
    std::string signalingUrl;
    std::string serverIp;
    std::string gpuType;
    std::vector<IceServer> iceServers;
    MediaConnectionInfo mediaConnectionInfo;
    std::uint32_t status = 0;
    std::uint32_t queuePosition = 0;
};

} // namespace opennow
