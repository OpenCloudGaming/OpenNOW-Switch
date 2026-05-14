#include "opennow/gfn/Auth.hpp"
#include "opennow/gfn/AuthService.hpp"
#include "opennow/gfn/AuthSessionStore.hpp"
#include "opennow/gfn/OAuthCallbackServer.hpp"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    opennow::gfn::AuthRequestBuilder builder;

    const auto provider = builder.defaultProvider();
    assert(provider.code == "NVIDIA");
    assert(provider.streamingServiceUrl == "https://prod.cloudmatchbeta.nvidiagrid.net/");

    auto normalized = builder.normalizeProvider({
        .idpId = "idp",
        .code = "CODE",
        .displayName = "Provider",
        .streamingServiceUrl = "https://example.test/base",
        .priority = 3,
    });
    assert(normalized.streamingServiceUrl == "https://example.test/base/");

    const auto authUrl = builder.buildAuthUrl({
        .provider = provider,
        .challenge = "abc 123",
        .redirectPort = 2259,
        .nonce = "nonce",
        .deviceId = "device-id",
    });
    assert(authUrl.find("https://login.nvidia.com/authorize?") == 0);
    assert(authUrl.find("response_type=code") != std::string::npos);
    assert(authUrl.find("scope=openid%20consent%20email%20tk_client%20age") != std::string::npos);
    assert(authUrl.find("code_challenge=abc%20123") != std::string::npos);
    assert(authUrl.find("redirect_uri=http%3A%2F%2Flocalhost%3A2259") != std::string::npos);
    assert(authUrl.find("idp_id=PDiAhv2kJTFeQ7WOPqiQ2tRZ7lGhR2X11dXvM4TZSxg") != std::string::npos);

    const auto exchange = builder.buildTokenExchangeRequest("code", "verifier", 6460);
    assert(exchange.method == opennow::net::HttpMethod::Post);
    assert(exchange.url == "https://login.nvidia.com/token");
    assert(exchange.headers.at("Content-Type") == "application/x-www-form-urlencoded; charset=UTF-8");
    assert(exchange.body.find("grant_type=authorization_code") != std::string::npos);
    assert(exchange.body.find("redirect_uri=http%3A%2F%2Flocalhost%3A6460") != std::string::npos);

    const auto deviceAuthorize = builder.buildDeviceAuthorizeRequest({
        .deviceId = "switch-device",
        .displayName = "OpenNOW Switch",
        .idpId = provider.idpId,
    });
    assert(deviceAuthorize.method == opennow::net::HttpMethod::Post);
    assert(deviceAuthorize.url == "https://login.nvidia.com/device/authorize");
    assert(deviceAuthorize.body.find("client_id=ZU7sPN-miLujMD95LfOQ453IB0AtjM8sMyvgJ9wCXEQ") != std::string::npos);
    assert(deviceAuthorize.body.find("scope=openid%20consent%20email%20tk_client%20age") != std::string::npos);
    assert(deviceAuthorize.body.find("device_id=switch-device") != std::string::npos);
    assert(deviceAuthorize.body.find("display_name=OpenNOW%20Switch") != std::string::npos);

    const auto deviceToken = builder.buildDeviceCodeTokenRequest("device-code");
    assert(deviceToken.url == "https://login.nvidia.com/token");
    assert(deviceToken.body.find("grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code") != std::string::npos);
    assert(deviceToken.body.find("device_code=device-code") != std::string::npos);

    const auto deviceInfo = opennow::gfn::parseDeviceAuthorizationResponse(R"({
        "device_code": "device-code",
        "user_code": "ABCD-EFGH",
        "verification_uri": "https://login.nvidia.com/activate",
        "verification_uri_complete": "https://login.nvidia.com/activate?user_code=ABCD-EFGH",
        "expires_in": 600,
        "interval": 5
    })");
    assert(deviceInfo.deviceCode == "device-code");
    assert(deviceInfo.userCode == "ABCD-EFGH");
    assert(deviceInfo.verificationUri.find("activate") != std::string::npos);
    assert(deviceInfo.expiresIn == 600);
    assert(deviceInfo.interval == 5);

    const auto challenge = opennow::gfn::buildCodeChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
    assert(challenge == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    const auto verifier = opennow::gfn::generateCodeVerifier();
    assert(verifier.size() >= 43);
    assert(verifier.find('=') == std::string::npos);

    opennow::net::MockHttpClient http;
    http.enqueue({200, {}, "{\"ok\":true}"});
    const auto response = http.send(exchange);
    assert(response.status == 200);
    assert(http.requests().size() == 1);
    assert(http.requests()[0].url == exchange.url);

    opennow::net::MockHttpClient loginHttp;
    loginHttp.enqueue({200, {}, R"({
        "access_token": "access",
        "refresh_token": "refresh",
        "id_token": "id",
        "expires_in": 3600
    })"});
    loginHttp.enqueue({200, {}, R"({
        "sub": "user-1",
        "preferred_username": "Player",
        "email": "player@example.com",
        "picture": "https://avatar.example/player.png",
        "gfn_tier": "ULTIMATE"
    })"});
    loginHttp.enqueue({200, {}, R"({
        "client_token": "client-token",
        "expires_in": 1800
    })"});

    opennow::gfn::AuthService service(loginHttp);
    const auto login = service.exchangeAuthorizationCode(provider, "code", "verifier", 2259);
    assert(login.ok);
    assert(login.session.provider.code == "NVIDIA");
    assert(login.session.tokens.accessToken == "access");
    assert(login.session.tokens.refreshToken == "refresh");
    assert(login.session.tokens.clientToken == "client-token");
    assert(login.session.user.userId == "user-1");
    assert(login.session.user.displayName == "Player");
    assert(login.session.user.membershipTier == "ULTIMATE");
    assert(loginHttp.requests().size() == 3);

    opennow::net::MockHttpClient deviceHttp;
    deviceHttp.enqueue({200, {}, R"({
        "device_code": "device-code",
        "user_code": "ABCD-EFGH",
        "verification_uri": "https://login.nvidia.com/activate",
        "verification_uri_complete": "https://login.nvidia.com/activate?user_code=ABCD-EFGH",
        "expires_in": 600,
        "interval": 5
    })"});
    deviceHttp.enqueue({400, {}, R"({"error":"authorization_pending","error_description":"pending"})"});
    deviceHttp.enqueue({400, {}, R"({"error":"slow_down","error_description":"slow down"})"});
    deviceHttp.enqueue({200, {}, R"({"access_token":"device-access","refresh_token":"device-refresh","expires_in":60})"});
    deviceHttp.enqueue({200, {}, R"({"sub":"user-device","preferred_username":"DeckFlow"})"});
    deviceHttp.enqueue({200, {}, R"({"client_token":"device-client","expires_in":60})"});
    opennow::gfn::AuthService deviceService(deviceHttp);
    const auto deviceAuthorizationResult = deviceService.requestDeviceAuthorization({
        .deviceId = "switch-device",
        .displayName = "OpenNOW Switch",
        .idpId = provider.idpId,
    });
    assert(deviceAuthorizationResult.ok);
    assert(deviceAuthorizationResult.authorization.userCode == "ABCD-EFGH");
    const auto pending = deviceService.pollDeviceCode(provider, deviceAuthorizationResult.authorization.deviceCode);
    assert(pending.status == opennow::gfn::DeviceCodePollStatus::Pending);
    const auto slowDown = deviceService.pollDeviceCode(provider, deviceAuthorizationResult.authorization.deviceCode);
    assert(slowDown.status == opennow::gfn::DeviceCodePollStatus::SlowDown);
    const auto deviceLogin = deviceService.pollDeviceCode(provider, deviceAuthorizationResult.authorization.deviceCode);
    assert(deviceLogin.status == opennow::gfn::DeviceCodePollStatus::Authorized);
    assert(deviceLogin.session.tokens.accessToken == "device-access");
    assert(deviceLogin.session.user.displayName == "DeckFlow");
    assert(deviceHttp.requests().size() == 6);

    std::string saveError;
    assert(opennow::gfn::saveAuthSession(deviceLogin.session, "auth-session-test.json", saveError));
    const auto loaded = opennow::gfn::loadAuthSession("auth-session-test.json");
    assert(loaded.ok);
    assert(loaded.session.tokens.refreshToken == "device-refresh");
    assert(loaded.session.user.displayName == "DeckFlow");
    std::remove("auth-session-test.json");

    opennow::net::MockHttpClient refreshHttp;
    refreshHttp.enqueue({200, {}, R"({"access_token":"new-access","expires_in":60})"});
    refreshHttp.enqueue({200, {}, R"({"sub":"user-1","preferred_username":"Player"})"});
    refreshHttp.enqueue({500, {}, "client token unavailable"});
    opennow::gfn::AuthService refreshService(refreshHttp);
    const auto refreshed = refreshService.refreshWithRefreshToken(login.session);
    assert(refreshed.ok);
    assert(refreshed.session.tokens.accessToken == "new-access");
    assert(refreshed.session.tokens.refreshToken == "refresh");

    const auto callback = opennow::gfn::OAuthCallbackServer::parseRequestTarget("/?code=abc%20123&state=x");
    assert(callback.ok);
    assert(callback.code == "abc 123");
    const auto callbackError = opennow::gfn::OAuthCallbackServer::parseRequestTarget("/?error=access_denied");
    assert(!callbackError.ok);
    assert(callbackError.error == "access_denied");

    return 0;
}
