#include "opennow/gfn/AuthService.hpp"

#include <chrono>
#include <sstream>
#include <utility>

#include "opennow/util/Json.hpp"

namespace opennow::gfn {

namespace {

bool isHttpOk(const net::HttpResponse& response) {
    return response.status >= 200 && response.status < 300;
}

std::string responseError(const char* action, const net::HttpResponse& response) {
    std::ostringstream out;
    out << action << " failed with HTTP " << response.status;
    if (!response.body.empty()) {
        out << ": " << response.body.substr(0, 300);
    }
    return out.str();
}

}

AuthService::AuthService(net::HttpClient& http)
    : AuthService(http, AuthRequestBuilder{}) {}

AuthService::AuthService(net::HttpClient& http, AuthRequestBuilder builder)
    : http_(http), builder_(std::move(builder)) {}

AuthResult AuthService::exchangeAuthorizationCode(
    const LoginProvider& provider,
    const std::string& code,
    const std::string& verifier,
    std::uint16_t redirectPort) {
    const auto tokenResponse = http_.send(builder_.buildTokenExchangeRequest(code, verifier, redirectPort));
    return sessionFromTokenResponse(provider, tokenResponse, "Token exchange");
}

AuthResult AuthService::refreshWithRefreshToken(const AuthSession& session) {
    if (session.tokens.refreshToken.empty()) {
        return {false, {}, "No refresh token available"};
    }

    const auto tokenResponse = http_.send(builder_.buildRefreshTokenRequest(session.tokens.refreshToken));
    if (!isHttpOk(tokenResponse)) {
        return {false, {}, responseError("Token refresh", tokenResponse)};
    }

    AuthSession refreshed = session;
    refreshed.tokens = parseTokenResponse(tokenResponse.body, unixTimeMs());
    if (refreshed.tokens.refreshToken.empty()) {
        refreshed.tokens.refreshToken = session.tokens.refreshToken;
    }
    if (refreshed.tokens.accessToken.empty()) {
        return {false, {}, "Token refresh response did not include access_token"};
    }

    const auto userResponse = http_.send(builder_.buildUserInfoRequest(refreshed.tokens.accessToken));
    if (isHttpOk(userResponse)) {
        auto user = parseUserInfoResponse(userResponse.body);
        if (!user.userId.empty()) {
            refreshed.user = std::move(user);
        }
    }

    tryAttachClientToken(refreshed);
    return {true, refreshed, {}};
}

AuthResult AuthService::refreshWithClientToken(const AuthSession& session) {
    if (session.tokens.clientToken.empty()) {
        return {false, {}, "No client token available"};
    }
    if (session.user.userId.empty()) {
        return {false, {}, "No NVIDIA user id available for client-token refresh"};
    }

    const auto tokenResponse = http_.send(builder_.buildClientTokenRefreshRequest(session.tokens.clientToken, session.user.userId));
    if (!isHttpOk(tokenResponse)) {
        return {false, {}, responseError("Client-token refresh", tokenResponse)};
    }

    AuthSession refreshed = session;
    refreshed.tokens = parseTokenResponse(tokenResponse.body, unixTimeMs());
    if (refreshed.tokens.refreshToken.empty()) {
        refreshed.tokens.refreshToken = session.tokens.refreshToken;
    }
    if (refreshed.tokens.clientToken.empty()) {
        refreshed.tokens.clientToken = session.tokens.clientToken;
        refreshed.tokens.clientTokenExpiresAtMs = session.tokens.clientTokenExpiresAtMs;
    }
    if (refreshed.tokens.accessToken.empty()) {
        return {false, {}, "Client-token refresh response did not include access_token"};
    }

    const auto userResponse = http_.send(builder_.buildUserInfoRequest(refreshed.tokens.accessToken));
    if (isHttpOk(userResponse)) {
        auto user = parseUserInfoResponse(userResponse.body);
        if (!user.userId.empty()) {
            refreshed.user = std::move(user);
        }
    }

    tryAttachClientToken(refreshed);
    return {true, refreshed, {}};
}

DeviceAuthorizationResult AuthService::requestDeviceAuthorization(const DeviceAuthorizationRequest& request) {
    const auto response = http_.send(builder_.buildDeviceAuthorizeRequest(request));
    if (!isHttpOk(response)) {
        return {false, {}, responseError("Device authorization", response)};
    }

    auto authorization = parseDeviceAuthorizationResponse(response.body);
    if (authorization.deviceCode.empty() || authorization.userCode.empty()
        || authorization.verificationUri.empty() || authorization.verificationUriComplete.empty()) {
        return {false, {}, "Device authorization response did not include display code data"};
    }

    return {true, authorization, {}};
}

AuthResult AuthService::exchangeDeviceCode(const LoginProvider& provider, const std::string& deviceCode) {
    const auto polled = pollDeviceCode(provider, deviceCode);
    if (polled.status == DeviceCodePollStatus::Authorized) {
        return {true, polled.session, {}};
    }
    return {false, {}, polled.error.empty() ? "Device authorization is not complete" : polled.error};
}

DeviceCodePollResult AuthService::pollDeviceCode(const LoginProvider& provider, const std::string& deviceCode) {
    const auto tokenResponse = http_.send(builder_.buildDeviceCodeTokenRequest(deviceCode));
    if (isHttpOk(tokenResponse)) {
        auto session = sessionFromTokenResponse(provider, tokenResponse, "Device token exchange");
        if (!session.ok) {
            return {DeviceCodePollStatus::Error, {}, session.error};
        }
        return {DeviceCodePollStatus::Authorized, session.session, {}};
    }

    const auto parsed = util::parseJson(tokenResponse.body);
    std::string errorCode;
    std::string description;
    if (parsed.ok) {
        if (const auto* value = parsed.value.get("error")) errorCode = value->asString();
        if (const auto* value = parsed.value.get("error_description")) description = value->asString();
    }

    const auto message = description.empty() ? responseError("Device token exchange", tokenResponse) : description;
    if (errorCode == "authorization_pending") {
        return {DeviceCodePollStatus::Pending, {}, message};
    }
    if (errorCode == "slow_down") {
        return {DeviceCodePollStatus::SlowDown, {}, message};
    }
    if (errorCode == "expired_token") {
        return {DeviceCodePollStatus::Expired, {}, message};
    }
    if (errorCode == "access_denied") {
        return {DeviceCodePollStatus::AccessDenied, {}, message};
    }

    return {DeviceCodePollStatus::Error, {}, responseError("Device token exchange", tokenResponse)};
}

AuthResult AuthService::sessionFromTokenResponse(const LoginProvider& provider, const net::HttpResponse& tokenResponse, const char* action) {
    if (!isHttpOk(tokenResponse)) {
        return {false, {}, responseError(action, tokenResponse)};
    }

    AuthSession session;
    session.provider = builder_.normalizeProvider(provider);
    session.tokens = parseTokenResponse(tokenResponse.body, unixTimeMs());
    if (session.tokens.accessToken.empty()) {
        return {false, {}, "Token exchange response did not include access_token"};
    }

    const auto userResponse = http_.send(builder_.buildUserInfoRequest(session.tokens.accessToken));
    if (!isHttpOk(userResponse)) {
        return {false, {}, responseError("User info request", userResponse)};
    }

    session.user = parseUserInfoResponse(userResponse.body);
    if (session.user.userId.empty()) {
        return {false, {}, "User info response did not include sub"};
    }

    tryAttachClientToken(session);
    return {true, session, {}};
}

AuthTokens AuthService::parseTokenResponse(const std::string& body, std::uint64_t nowMs) const {
    AuthTokens tokens;
    const auto parsed = util::parseJson(body);
    if (!parsed.ok) {
        return tokens;
    }

    if (const auto* value = parsed.value.get("access_token")) tokens.accessToken = value->asString();
    if (tokens.accessToken.empty()) {
        if (const auto* value = parsed.value.get("accessToken")) tokens.accessToken = value->asString();
    }
    if (const auto* value = parsed.value.get("refresh_token")) tokens.refreshToken = value->asString();
    if (tokens.refreshToken.empty()) {
        if (const auto* value = parsed.value.get("refreshToken")) tokens.refreshToken = value->asString();
    }
    if (const auto* value = parsed.value.get("id_token")) tokens.idToken = value->asString();
    if (tokens.idToken.empty()) {
        if (const auto* value = parsed.value.get("idToken")) tokens.idToken = value->asString();
    }
    if (const auto* value = parsed.value.get("client_token")) tokens.clientToken = value->asString();
    if (tokens.clientToken.empty()) {
        if (const auto* value = parsed.value.get("clientToken")) tokens.clientToken = value->asString();
    }
    const auto expiresIn = parsed.value.get("expires_in")
        ? static_cast<std::uint64_t>(parsed.value.get("expires_in")->asNumber())
        : 86400ULL;
    tokens.expiresAtMs = nowMs + expiresIn * 1000ULL;
    return tokens;
}

AuthUser AuthService::parseUserInfoResponse(const std::string& body) const {
    AuthUser user;
    const auto parsed = util::parseJson(body);
    if (!parsed.ok) {
        return user;
    }

    if (const auto* value = parsed.value.get("sub")) user.userId = value->asString();
    if (const auto* value = parsed.value.get("preferred_username")) user.displayName = value->asString();
    if (const auto* value = parsed.value.get("email")) user.email = value->asString();
    if (const auto* value = parsed.value.get("picture")) user.avatarUrl = value->asString();
    if (const auto* value = parsed.value.get("gfn_tier")) user.membershipTier = value->asString("FREE");
    if (user.displayName.empty() && !user.email.empty()) {
        const auto at = user.email.find('@');
        user.displayName = user.email.substr(0, at);
    }
    if (user.displayName.empty()) {
        user.displayName = "User";
    }
    return user;
}

void AuthService::tryAttachClientToken(AuthSession& session) {
    const auto response = http_.send(builder_.buildClientTokenRequest(session.tokens.accessToken));
    if (!isHttpOk(response)) {
        return;
    }

    const auto parsed = util::parseJson(response.body);
    if (!parsed.ok) {
        return;
    }

    if (const auto* token = parsed.value.get("client_token")) {
        session.tokens.clientToken = token->asString();
    }
    if (const auto* expires = parsed.value.get("expires_in")) {
        session.tokens.clientTokenExpiresAtMs = unixTimeMs() + static_cast<std::uint64_t>(expires->asNumber()) * 1000ULL;
    }
}

std::uint64_t unixTimeMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace opennow::gfn
