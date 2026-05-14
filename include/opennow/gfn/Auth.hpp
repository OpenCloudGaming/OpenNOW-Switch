#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "opennow/gfn/GfnTypes.hpp"
#include "opennow/net/HttpClient.hpp"

namespace opennow::gfn {

struct AuthEndpoints {
    std::string serviceUrls = "https://pcs.geforcenow.com/v1/serviceUrls";
    std::string token = "https://login.nvidia.com/token";
    std::string clientToken = "https://login.nvidia.com/client_token";
    std::string userInfo = "https://login.nvidia.com/userinfo";
    std::string authorize = "https://login.nvidia.com/authorize";
    std::string deviceAuthorize = "https://login.nvidia.com/device/authorize";
};

struct AuthUrlRequest {
    LoginProvider provider;
    std::string challenge;
    std::uint16_t redirectPort = 2259;
    std::string nonce;
    std::string deviceId;
};

struct DeviceAuthorizationRequest {
    std::string deviceId;
    std::string displayName = "OpenNOW Switch";
    std::string idpId;
};

struct DeviceAuthorizationInfo {
    std::string deviceCode;
    std::string userCode;
    std::string verificationUri;
    std::string verificationUriComplete;
    std::uint32_t expiresIn = 0;
    std::uint32_t interval = 5;
};

enum class AuthClientProfile {
    Desktop,
    SteamDeck,
};

class AuthRequestBuilder {
public:
    explicit AuthRequestBuilder(
        AuthEndpoints endpoints = {},
        AuthClientProfile profile = AuthClientProfile::Desktop);
    static AuthRequestBuilder steamDeck(AuthEndpoints endpoints = {});

    LoginProvider defaultProvider() const;
    LoginProvider normalizeProvider(LoginProvider provider) const;
    const char* profileName() const;

    std::string buildAuthUrl(const AuthUrlRequest& request) const;
    std::string buildTokenExchangeBody(const std::string& code, const std::string& verifier, std::uint16_t redirectPort) const;
    std::string buildRefreshTokenBody(const std::string& refreshToken) const;
    std::string buildClientTokenRefreshBody(const std::string& clientToken, const std::string& userId) const;
    std::string buildDeviceAuthorizeBody(const DeviceAuthorizationRequest& request) const;
    std::string buildDeviceCodeTokenBody(const std::string& deviceCode) const;

    net::HttpRequest buildServiceUrlsRequest() const;
    net::HttpRequest buildTokenExchangeRequest(const std::string& code, const std::string& verifier, std::uint16_t redirectPort) const;
    net::HttpRequest buildRefreshTokenRequest(const std::string& refreshToken) const;
    net::HttpRequest buildClientTokenRefreshRequest(const std::string& clientToken, const std::string& userId) const;
    net::HttpRequest buildDeviceAuthorizeRequest(const DeviceAuthorizationRequest& request) const;
    net::HttpRequest buildDeviceCodeTokenRequest(const std::string& deviceCode) const;
    net::HttpRequest buildUserInfoRequest(const std::string& accessToken) const;
    net::HttpRequest buildClientTokenRequest(const std::string& accessToken) const;

private:
    net::Headers authHeaders(bool includeContentType) const;

    AuthEndpoints endpoints_;
    AuthClientProfile profile_ = AuthClientProfile::Desktop;
};

std::string urlEncode(std::string_view value);
std::string base64UrlEncode(const std::uint8_t* data, std::size_t size);
std::string generateCodeVerifier(std::size_t byteCount = 32);
std::string buildCodeChallenge(std::string_view verifier);
std::string generateOpaqueToken(std::size_t byteCount = 16);
DeviceAuthorizationInfo parseDeviceAuthorizationResponse(const std::string& body);

} // namespace opennow::gfn
