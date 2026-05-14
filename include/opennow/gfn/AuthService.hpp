#pragma once

#include <cstdint>
#include <string>

#include "opennow/gfn/Auth.hpp"
#include "opennow/gfn/GfnTypes.hpp"
#include "opennow/net/HttpClient.hpp"

namespace opennow::gfn {

struct AuthResult {
    bool ok = false;
    AuthSession session;
    std::string error;
};

struct DeviceAuthorizationResult {
    bool ok = false;
    DeviceAuthorizationInfo authorization;
    std::string error;
};

enum class DeviceCodePollStatus {
    Authorized,
    Pending,
    SlowDown,
    Expired,
    AccessDenied,
    Error,
};

struct DeviceCodePollResult {
    DeviceCodePollStatus status = DeviceCodePollStatus::Error;
    AuthSession session;
    std::string error;
};

class AuthService {
public:
    explicit AuthService(net::HttpClient& http);
    AuthService(net::HttpClient& http, AuthRequestBuilder builder);

    AuthResult exchangeAuthorizationCode(
        const LoginProvider& provider,
        const std::string& code,
        const std::string& verifier,
        std::uint16_t redirectPort);

    AuthResult refreshWithRefreshToken(const AuthSession& session);
    AuthResult refreshWithClientToken(const AuthSession& session);
    DeviceAuthorizationResult requestDeviceAuthorization(const DeviceAuthorizationRequest& request);
    AuthResult exchangeDeviceCode(const LoginProvider& provider, const std::string& deviceCode);
    DeviceCodePollResult pollDeviceCode(const LoginProvider& provider, const std::string& deviceCode);

private:
    AuthResult sessionFromTokenResponse(const LoginProvider& provider, const net::HttpResponse& tokenResponse, const char* action);
    AuthTokens parseTokenResponse(const std::string& body, std::uint64_t nowMs) const;
    AuthUser parseUserInfoResponse(const std::string& body) const;
    void tryAttachClientToken(AuthSession& session);

    net::HttpClient& http_;
    AuthRequestBuilder builder_;
};

std::uint64_t unixTimeMs();

} // namespace opennow::gfn
