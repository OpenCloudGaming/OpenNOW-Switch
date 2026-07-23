#include "internal.hpp"
#include "persistence_internal.hpp"

#include "../auth_policy.hpp"
#include "../server_location_policy.hpp"
#include "../stream_settings.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace opennow
{
using namespace gfn::detail;

namespace
{
constexpr const char* kTokenEndpoint       = "https://login.nvidia.com/token";
constexpr const char* kDeviceAuthorizeEndpoint = "https://login.nvidia.com/device/authorize";
constexpr const char* kClientTokenEndpoint = "https://login.nvidia.com/client_token";
constexpr const char* kUserInfoEndpoint    = "https://login.nvidia.com/userinfo";
constexpr const char* kSubscriptionEndpoint = "https://mes.geforcenow.com/v4/subscriptions";
constexpr const char* kClientId            = "ZU7sPN-miLujMD95LfOQ453IB0AtjM8sMyvgJ9wCXEQ";
constexpr const char* kDeviceClientId      = "q61ddeJrVt7O90Nl-P-N7I36yctih4Ml6FyXLrb6j-U";
constexpr const char* kLcarsClientId       = "ec7e38d4-03af-4b58-b131-cfb0495903ab";
constexpr const char* kGfnClientVersion    = "2.0.80.173";
constexpr const char* kScopes              = "openid consent email tk_client age";
constexpr const char* kDeviceUserAgent     =
    "Mozilla/5.0 (X11; Linux x86_64; Steam Deck) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";
constexpr const char* kNvidiaFileOrigin    = "https://nvfile";
constexpr const char* kNvidiaFileReferer   = "https://nvfile/";
constexpr std::int64_t kRefreshWindowMs    = 10LL * 60LL * 1000LL;
constexpr std::int64_t kMembershipRefreshIntervalMs = 60LL * 60LL * 1000LL;
constexpr auto kQrLoginTimeout             = std::chrono::minutes(5);
std::string AuthClientId(const AuthTokens& tokens);
const char* AuthUserAgent(const AuthTokens& tokens);
std::vector<std::string> BuildAuthHeadersForTokens(
    const AuthTokens& tokens,
    const std::string& bearer_token,
    const std::string& content_type,
    const std::string& accept);


std::vector<std::string> BuildNvidiaAuthHeaders(
    const std::string& bearer_token = {},
    const std::string& content_type = {},
    bool include_referer            = false,
    const std::string& accept       = "application/json, text/plain, */*")
{
    std::vector<std::string> headers;
    headers.push_back("Origin: " + std::string(kNvidiaFileOrigin));
    headers.push_back("Accept: " + accept);
    headers.push_back("User-Agent: " + std::string(GfnClient::kUserAgent));

    if (!bearer_token.empty())
        headers.push_back("Authorization: Bearer " + bearer_token);

    if (!content_type.empty())
        headers.push_back("Content-Type: " + content_type);

    if (include_referer)
        headers.push_back("Referer: " + std::string(kNvidiaFileReferer));

    return headers;
}

std::int64_t ToExpiresAtMs(json_t* payload, const char* key, std::int64_t fallback_ms = 24LL * 60LL * 60LL * 1000LL)
{
    if (payload && json_is_object(payload))
    {
        json_t* expires = json_object_get(payload, key);
        if (json_is_integer(expires))
            return NowMs() + static_cast<std::int64_t>(json_integer_value(expires)) * 1000LL;
    }

    return NowMs() + fallback_ms;
}

bool IsNearExpiry(std::int64_t expires_at_ms)
{
    return auth::ShouldRefresh(expires_at_ms, NowMs(), kRefreshWindowMs);
}

bool IsExpired(std::int64_t expires_at_ms)
{
    return auth::IsExpired(expires_at_ms, NowMs());
}


AuthTokens ParseAuthTokens(json_t* payload, const std::string& auth_client_id = kClientId)
{
    AuthTokens tokens;
    tokens.access_token              = GetString(payload, "access_token");
    tokens.refresh_token             = GetString(payload, "refresh_token");
    tokens.id_token                  = GetString(payload, "id_token");
    tokens.client_token              = GetString(payload, "client_token");
    tokens.auth_client_id            = auth_client_id;
    tokens.expires_at_ms             = ToExpiresAtMs(payload, "expires_in");
    tokens.client_token_expires_at_ms = ToExpiresAtMs(payload, "expires_in");

    if (tokens.access_token.empty())
        throw std::runtime_error("Login succeeded but no access token was returned");

    return tokens;
}


AuthTokens RefreshTokens(const HttpClient& http_client, const AuthSession& session)
{
    if (session.tokens.refresh_token.empty())
        throw ReauthenticationRequired("Saved GeForce NOW session cannot be refreshed. Please sign in again.");

    const std::string body =
        "grant_type=refresh_token"
        "&refresh_token=" + UrlEncode(session.tokens.refresh_token, true) +
        "&client_id=" + UrlEncode(AuthClientId(session.tokens), true);

    HttpResponse response;
    for (int attempt = 1; attempt <= 3; ++attempt)
    {
        response = http_client.Post(
            kTokenEndpoint,
            AuthUserAgent(session.tokens),
            BuildAuthHeadersForTokens(
                session.tokens, {},
                "application/x-www-form-urlencoded; charset=UTF-8",
                "application/json, text/plain, */*"),
            body);
        const bool temporary = auth::IsTemporaryHttpStatus(response.status_code);
        if (response.status_code == 200 || !temporary || attempt == 3)
            break;

        AppendAuthLog("auth: token refresh temporary failure HTTP " +
                      std::to_string(response.status_code) +
                      " retry=" + std::to_string(attempt));
        std::this_thread::sleep_for(
            std::chrono::milliseconds(auth::RefreshRetryDelayMs(attempt)));
    }

    if (response.status_code != 200)
    {
        AppendAuthLog("auth: token refresh failed HTTP " + std::to_string(response.status_code));
        if (response.status_code == 400 || response.status_code == 401)
            throw ReauthenticationRequired("Saved GeForce NOW login is no longer valid. Please sign in again.");
        throw std::runtime_error(
            "Could not refresh the saved login (HTTP " + std::to_string(response.status_code) + "). Please try again.");
    }

    JsonPtr root      = LoadJson(response.body);
    AuthTokens tokens = ParseAuthTokens(root.get(), AuthClientId(session.tokens));
    if (tokens.refresh_token.empty())
        tokens.refresh_token = session.tokens.refresh_token;

    // NVIDIA commonly omits id_token during OAuth refresh. CloudMatch requires
    // the signed JWT, so an omitted field must not erase the last usable token.
    if (tokens.id_token.empty())
        tokens.id_token = session.tokens.id_token;

    if (tokens.client_token.empty())
    {
        tokens.client_token              = session.tokens.client_token;
        tokens.client_token_expires_at_ms = session.tokens.client_token_expires_at_ms;
    }

    return tokens;
}

std::vector<std::string> BuildDeviceAuthHeaders(
    const std::string& device_id = {}, bool include_device_identity = false)
{
    std::vector<std::string> headers = {
        "Origin: https://play.geforcenow.com",
        "Referer: https://play.geforcenow.com/",
        "Accept: application/json, text/plain, */*",
        "Content-Type: application/x-www-form-urlencoded; charset=UTF-8",
    };
    if (!include_device_identity)
        return headers;

    headers.push_back("x-device-id: " + device_id);
    headers.push_back("nv-client-id: " + std::string(kDeviceClientId));
    headers.push_back("nv-client-streamer: WEBRTC");
    headers.push_back("nv-client-type: BROWSER");
    headers.push_back("nv-client-platform-name: browser");
    headers.push_back("nv-browser-type: CHROME");
    headers.push_back("nv-device-os: STEAMOS");
    headers.push_back("nv-device-type: CONSOLE");
    headers.push_back("nv-device-model: STEAMDECK");
    headers.push_back("nv-device-make: VALVE");
    return headers;
}

std::string AuthClientId(const AuthTokens& tokens)
{
    return tokens.auth_client_id.empty() ? std::string(kClientId) : tokens.auth_client_id;
}

const char* AuthUserAgent(const AuthTokens& tokens)
{
    return AuthClientId(tokens) == kDeviceClientId
        ? kDeviceUserAgent
        : GfnClient::kUserAgent;
}

std::vector<std::string> BuildAuthHeadersForTokens(
    const AuthTokens& tokens,
    const std::string& bearer_token,
    const std::string& content_type,
    const std::string& accept)
{
    if (AuthClientId(tokens) != kDeviceClientId)
        return BuildNvidiaAuthHeaders(bearer_token, content_type, false, accept);

    std::vector<std::string> headers = {
        "Origin: https://play.geforcenow.com",
        "Referer: https://play.geforcenow.com/",
        "Accept: " + accept,
    };
    if (!bearer_token.empty())
        headers.push_back("Authorization: Bearer " + bearer_token);
    if (!content_type.empty())
        headers.push_back("Content-Type: " + content_type);
    return headers;
}

AuthTokens RefreshTokensWithClientToken(
    const HttpClient& http_client, const AuthSession& session)
{
    if (session.tokens.client_token.empty() || session.user.user_id.empty())
        throw std::runtime_error("Saved session has no client-token refresh mechanism");

    const std::string body =
        "grant_type=" + UrlEncode("urn:ietf:params:oauth:grant-type:client_token", true) +
        "&client_token=" + UrlEncode(session.tokens.client_token, true) +
        "&client_id=" + UrlEncode(AuthClientId(session.tokens), true) +
        "&sub=" + UrlEncode(session.user.user_id, true);

    const HttpResponse response = http_client.Post(
        kTokenEndpoint,
        AuthUserAgent(session.tokens),
        BuildAuthHeadersForTokens(
            session.tokens, {},
            "application/x-www-form-urlencoded; charset=UTF-8",
            "application/json, text/plain, */*"),
        body);
    if (response.status_code != 200)
    {
        throw std::runtime_error(
            "Client-token refresh failed with HTTP " + std::to_string(response.status_code));
    }

    JsonPtr root = LoadJson(response.body);
    AuthTokens tokens = ParseAuthTokens(root.get(), AuthClientId(session.tokens));
    if (tokens.refresh_token.empty())
        tokens.refresh_token = session.tokens.refresh_token;
    if (tokens.id_token.empty())
        tokens.id_token = session.tokens.id_token;
    if (tokens.client_token.empty())
    {
        tokens.client_token = session.tokens.client_token;
        tokens.client_token_expires_at_ms = session.tokens.client_token_expires_at_ms;
    }
    else if (tokens.client_token != session.tokens.client_token)
    {
        // A rotated client token needs fresh lifetime metadata from /client_token.
        tokens.client_token_expires_at_ms = 0;
    }
    return tokens;
}

AuthTokens RefreshSessionTokens(const HttpClient& http_client, const AuthSession& session)
{
    if (!session.tokens.client_token.empty())
    {
        try
        {
            AuthTokens refreshed = RefreshTokensWithClientToken(http_client, session);
            AppendAuthLog("auth: client-token refresh ok");
            return refreshed;
        }
        catch (const std::exception& e)
        {
            AppendAuthLog("auth: client-token refresh failed; trying OAuth refresh error=" +
                          std::string(e.what()));
        }
    }

    return RefreshTokens(http_client, session);
}

void RequestClientToken(const HttpClient& http_client, AuthTokens& tokens)
{
    if (tokens.access_token.empty())
        return;

    const HttpResponse response = http_client.Get(
        kClientTokenEndpoint,
        AuthUserAgent(tokens),
        BuildAuthHeadersForTokens(
            tokens, tokens.access_token, {},
            "application/json, text/plain, */*"));

    if (response.status_code != 200)
        return;

    JsonPtr root = LoadJson(response.body);
    const std::string client_token = GetString(root.get(), "client_token");
    if (client_token.empty())
        return;

    tokens.client_token               = client_token;
    tokens.client_token_expires_at_ms = ToExpiresAtMs(root.get(), "expires_in");
}

AuthUser FetchUserInfo(const HttpClient& http_client, const AuthTokens& tokens)
{
    const HttpResponse response = http_client.Get(
        kUserInfoEndpoint,
        AuthUserAgent(tokens),
        BuildAuthHeadersForTokens(tokens, tokens.access_token, {}, "application/json"));

    if (response.status_code != 200)
    {
        throw std::runtime_error(
            "Failed to fetch user info with HTTP " + std::to_string(response.status_code));
    }

    JsonPtr root = LoadJson(response.body);
    AuthUser user;
    user.user_id         = GetString(root.get(), "sub");
    user.display_name    = GetString(root.get(), "preferred_username");
    user.email           = GetString(root.get(), "email");
    user.avatar_url      = GetString(root.get(), "picture");
    user.membership_tier.clear();
    user.membership_tier_verified = false;

    if (user.display_name.empty() && !user.email.empty())
    {
        const auto at = user.email.find('@');
        user.display_name =
            at == std::string::npos ? user.email : user.email.substr(0, at);
    }

    if (user.display_name.empty())
        user.display_name = "GFN User";

    if (user.user_id.empty())
        throw std::runtime_error("Login succeeded but user info is incomplete");

    return user;
}

std::vector<std::string> BuildMembershipHeaders(const std::string& token)
{
    return {
        "Accept: application/json",
        "Authorization: GFNJWT " + token,
        "nv-client-id: " + std::string(kLcarsClientId),
        "nv-client-type: NATIVE",
        "nv-client-version: " + std::string(kGfnClientVersion),
        "nv-client-streamer: NVIDIA-CLASSIC",
        "nv-device-os: WINDOWS",
        "nv-device-type: DESKTOP",
        "nv-device-make: UNKNOWN",
        "nv-device-model: UNKNOWN",
    };
}

double JsonNumber(json_t* object, const char* key, double fallback = 0.0)
{
    if (!object || !json_is_object(object))
        return fallback;

    json_t* value = json_object_get(object, key);
    if (json_is_integer(value))
        return static_cast<double>(json_integer_value(value));
    if (json_is_real(value))
        return json_real_value(value);
    if (!json_is_string(value))
        return fallback;

    const char* text = json_string_value(value);
    if (!text || !*text)
        return fallback;
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    return end != text && std::isfinite(parsed) ? parsed : fallback;
}

std::string AddonAttribute(json_t* addon, const char* key)
{
    json_t* attributes = addon ? json_object_get(addon, "attributes") : nullptr;
    if (!json_is_array(attributes))
        return {};

    size_t index;
    json_t* attribute;
    json_array_foreach(attributes, index, attribute)
    {
        if (GetString(attribute, "key") == key)
            return GetString(attribute, "textValue");
    }
    return {};
}

bool ParseNumberText(const std::string& text, double& output)
{
    if (text.empty())
        return false;
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(parsed))
        return false;
    output = parsed;
    return true;
}

SubscriptionInfo ParseSubscription(json_t* root)
{
    SubscriptionInfo info;
    info.available            = true;
    info.membership_tier      = Trim(GetString(root, "membershipTier"));
    info.subscription_type    = GetString(root, "type");
    info.subscription_subtype = GetString(root, "subType");

    const double allotted_minutes = JsonNumber(root, "allottedTimeInMinutes");
    const double purchased_minutes = JsonNumber(root, "purchasedTimeInMinutes");
    const double rolled_over_minutes = JsonNumber(root, "rolledOverTimeInMinutes");
    const double fallback_total = allotted_minutes + purchased_minutes + rolled_over_minutes;
    const double total_minutes = JsonNumber(root, "totalTimeInMinutes", fallback_total);
    const double remaining_minutes = JsonNumber(root, "remainingTimeInMinutes");

    info.allotted_hours    = allotted_minutes / 60.0;
    info.purchased_hours   = purchased_minutes / 60.0;
    info.rolled_over_hours = rolled_over_minutes / 60.0;
    info.total_hours       = total_minutes / 60.0;
    info.remaining_hours   = remaining_minutes / 60.0;
    info.used_hours        = std::max(total_minutes - remaining_minutes, 0.0) / 60.0;
    info.is_unlimited      = info.subscription_subtype == "UNLIMITED";

    json_t* addons = root ? json_object_get(root, "addons") : nullptr;
    if (!json_is_array(addons))
        return info;

    size_t index;
    json_t* addon;
    json_array_foreach(addons, index, addon)
    {
        if (GetString(addon, "type") != "STORAGE" ||
            GetString(addon, "subType") != "PERMANENT_STORAGE" ||
            GetString(addon, "status") != "OK")
            continue;

        info.has_storage = true;
        const bool has_size = ParseNumberText(
            AddonAttribute(addon, "TOTAL_STORAGE_SIZE_IN_GB"), info.storage_size_gb);
        const bool has_used = ParseNumberText(
            AddonAttribute(addon, "USED_STORAGE_SIZE_IN_GB"), info.storage_used_gb);
        info.has_storage_usage = has_size && has_used;
        info.storage_region_name = AddonAttribute(addon, "STORAGE_METRO_REGION_NAME");
        info.storage_region_code = AddonAttribute(addon, "STORAGE_METRO_REGION");
        break;
    }
    return info;
}

std::string FetchVpcId(const HttpClient& http_client, const AuthSession& session)
{
    const std::string token = ResolveSessionJwt(session);
    if (token.empty())
        return "NP-AMS-08";

    const StreamSettings settings = LoadStreamSettings();
    const std::string base_url = server_location::ResolveStreamingBaseUrl(
        settings.region, session.provider.streaming_service_url);
    if (base_url.empty())
        return "NP-AMS-08";
    const std::string url = base_url + "v2/serverInfo";
    std::vector<std::string> headers = BuildMembershipHeaders(token);
    for (std::string& header : headers)
    {
        if (header == "nv-client-type: NATIVE")
            header = "nv-client-type: BROWSER";
        else if (header == "nv-client-streamer: NVIDIA-CLASSIC")
            header = "nv-client-streamer: WEBRTC";
    }

    const HttpResponse response = http_client.Get(url, GfnClient::kUserAgent, headers);
    if (response.status_code != 200)
        return "NP-AMS-08";

    try
    {
        JsonPtr root = LoadJson(response.body);
        const std::string vpc = GetString(json_object_get(root.get(), "requestStatus"), "serverId");
        return vpc.empty() ? "NP-AMS-08" : vpc;
    }
    catch (...)
    {
        return "NP-AMS-08";
    }
}

bool RefreshMembershipTier(const HttpClient& http_client, AuthSession& session)
{
    session.membership_checked_at_ms = NowMs();
    const std::string token = ResolveSessionJwt(session);
    if (token.empty() || session.user.user_id.empty())
        return false;

    try
    {
        const std::string vpc_id = FetchVpcId(http_client, session);
        const std::string url = std::string(kSubscriptionEndpoint) +
            "?serviceName=gfn_pc&languageCode=en_US&vpcId=" + UrlEncode(vpc_id) +
            "&userId=" + UrlEncode(session.user.user_id);
        const HttpResponse response = http_client.Get(
            url, GfnClient::kUserAgent, BuildMembershipHeaders(token));
        if (response.status_code != 200)
        {
            AppendAuthLog("auth: subscription lookup failed HTTP " +
                          std::to_string(response.status_code));
            return false;
        }

        JsonPtr root = LoadJson(response.body);
        session.subscription = ParseSubscription(root.get());
        const std::string tier = session.subscription.membership_tier;
        if (tier.empty())
            return false;
        session.user.membership_tier = tier;
        session.user.membership_tier_verified = true;
        AppendAuthLog("auth: subscription tier resolved");
        return true;
    }
    catch (const std::exception& e)
    {
        AppendAuthLog("auth: subscription response parse failed error=" + std::string(e.what()));
        return false;
    }
}


} // namespace




AuthSession GfnClient::RecoverSavedSession(
    const AuthSession& session,
    bool force_refresh) const
{
    if (session.reauthentication_required)
        throw ReauthenticationRequired(
            "The NVIDIA session expired. Reconnect this account with a QR code.");

    try
    {
        AuthSession recovered = force_refresh
            ? ForceRefreshSavedSession(session)
            : EnsureFreshSavedSession(session);
        recovered.reauthentication_required = false;
        return recovered;
    }
    catch (const ReauthenticationRequired&)
    {
        throw ReauthenticationRequired(
            "The NVIDIA session expired. Reconnect this account with a QR code.");
    }
}

AuthSession GfnClient::LoginWithQrCode(
    const LoginProvider& provider,
    const std::function<void(const QrLoginChallenge&)>& on_challenge,
    const std::function<bool()>& is_cancelled) const
{
    AppendAuthLog("auth-device: login started provider=" + provider.code);

    const std::string device_id = GenerateDeviceId();
    const std::string authorization_body =
        "client_id=" + UrlEncode(kDeviceClientId, true) +
        "&scope=" + UrlEncode(kScopes, true) +
        "&device_id=" + UrlEncode(device_id, true) +
        "&display_name=" + UrlEncode("OpenNOW", true) +
        "&idp_id=" + UrlEncode(provider.idp_id, true);
    const HttpResponse authorization = http_client_.Post(
        kDeviceAuthorizeEndpoint,
        kDeviceUserAgent,
        BuildDeviceAuthHeaders(device_id, true),
        authorization_body);
    if (authorization.status_code != 200)
    {
        AppendAuthLog(
            "auth-device: authorization failed HTTP " +
            std::to_string(authorization.status_code));
        throw std::runtime_error(
            "QR authorization failed with HTTP " +
            std::to_string(authorization.status_code));
    }

    JsonPtr challenge_json = LoadJson(authorization.body);
    const std::string device_code = GetString(challenge_json.get(), "device_code");
    QrLoginChallenge challenge;
    challenge.user_code = GetString(challenge_json.get(), "user_code");
    challenge.verification_uri = GetString(challenge_json.get(), "verification_uri");
    challenge.verification_uri_complete =
        GetString(challenge_json.get(), "verification_uri_complete");
    challenge.expires_at_ms = ToExpiresAtMs(
        challenge_json.get(), "expires_in",
        std::chrono::duration_cast<std::chrono::milliseconds>(kQrLoginTimeout).count());
    challenge.interval_seconds = std::max(
        1, GetInteger(challenge_json.get(), "interval", 5));
    if (device_code.empty() || challenge.user_code.empty() ||
        challenge.verification_uri.empty() || challenge.verification_uri_complete.empty())
    {
        throw std::runtime_error(
            "NVIDIA did not return the data required for QR login");
    }

    if (on_challenge)
        on_challenge(challenge);

    int poll_interval_seconds = challenge.interval_seconds;
    while (NowMs() < challenge.expires_at_ms)
    {
        const auto wait_until = std::chrono::steady_clock::now() +
            std::chrono::seconds(poll_interval_seconds);
        while (std::chrono::steady_clock::now() < wait_until)
        {
            if (is_cancelled && is_cancelled())
                throw std::runtime_error("QR login was cancelled");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        const std::string token_body =
            "grant_type=" + UrlEncode(
                "urn:ietf:params:oauth:grant-type:device_code", true) +
            "&device_code=" + UrlEncode(device_code, true) +
            "&client_id=" + UrlEncode(kDeviceClientId, true);
        const HttpResponse token_response = http_client_.Post(
            kTokenEndpoint,
            kDeviceUserAgent,
            BuildDeviceAuthHeaders(),
            token_body);
        JsonPtr token_json = LoadJson(token_response.body);
        if (token_response.status_code == 200)
        {
            AuthTokens tokens = ParseAuthTokens(token_json.get(), kDeviceClientId);
            if (tokens.client_token.empty())
                RequestClientToken(http_client_, tokens);

            AuthSession session;
            session.provider = provider;
            session.tokens = std::move(tokens);
            session.user = FetchUserInfo(http_client_, session.tokens);
            session.last_refresh_at_ms = NowMs();
            RefreshMembershipTier(http_client_, session);
            AppendAuthLog("auth-device: login complete");
            return session;
        }

        const std::string error = GetString(token_json.get(), "error");
        if (error == "authorization_pending")
            continue;
        if (error == "slow_down")
        {
            poll_interval_seconds += 5;
            continue;
        }

        const std::string description = GetString(token_json.get(), "error_description");
        if (error == "expired_token")
            throw std::runtime_error("The QR login code expired. Please try again.");
        if (error == "access_denied")
            throw std::runtime_error("The QR login request was denied.");
        throw std::runtime_error(
            description.empty() ? "QR login failed" : description);
    }

    throw std::runtime_error("The QR login code expired. Please try again.");
}

AuthSession GfnClient::EnsureFreshSession(const AuthSession& session) const
{
    const bool access_needs_refresh = IsNearExpiry(session.tokens.expires_at_ms);
    const bool client_needs_refresh = session.tokens.client_token.empty() ||
        IsNearExpiry(session.tokens.client_token_expires_at_ms);
    const bool membership_needs_refresh =
        session.membership_checked_at_ms <= 0 ||
        NowMs() - session.membership_checked_at_ms >= kMembershipRefreshIntervalMs;

    if (!access_needs_refresh && !client_needs_refresh && !membership_needs_refresh)
        return session;

    AuthSession refreshed = session;

    if (access_needs_refresh && session.tokens.refresh_token.empty())
    {
        if (IsExpired(session.tokens.expires_at_ms))
            throw ReauthenticationRequired("Saved GeForce NOW login has expired. Please sign in again.");
    }
    else if (access_needs_refresh)
    {
        AppendAuthLog("auth: refreshing access token user_id_present=" +
                      std::string(session.user.user_id.empty() ? "no" : "yes"));
        refreshed.tokens = RefreshSessionTokens(http_client_, session);
        refreshed.last_refresh_at_ms = NowMs();
        AppendAuthLog("auth: access token refresh ok");
    }

    if (refreshed.tokens.client_token.empty() ||
        IsNearExpiry(refreshed.tokens.client_token_expires_at_ms))
    {
        RequestClientToken(http_client_, refreshed.tokens);
    }
    if (membership_needs_refresh || access_needs_refresh)
        RefreshMembershipTier(http_client_, refreshed);
    return refreshed;
}

AuthSession GfnClient::EnsureFreshSavedSession(const AuthSession& session) const
{
    if (!session.persistence_enabled)
        return EnsureFreshSession(session);

    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    AuthSession source = session;
    for (const AuthSession& saved : LoadAccountsFromDisk(nullptr))
    {
        if (saved.user.user_id == session.user.user_id &&
            (saved.last_refresh_at_ms > source.last_refresh_at_ms ||
             saved.tokens.expires_at_ms > source.tokens.expires_at_ms))
        {
            source = saved;
        }
    }

    AuthSession refreshed = EnsureFreshSession(source);
    const bool changed =
        refreshed.tokens.access_token != source.tokens.access_token ||
        refreshed.tokens.refresh_token != source.tokens.refresh_token ||
        refreshed.tokens.id_token != source.tokens.id_token ||
        refreshed.tokens.client_token != source.tokens.client_token ||
        refreshed.tokens.expires_at_ms != source.tokens.expires_at_ms ||
        refreshed.tokens.client_token_expires_at_ms != source.tokens.client_token_expires_at_ms ||
        refreshed.user.membership_tier != source.user.membership_tier ||
        refreshed.user.membership_tier_verified != source.user.membership_tier_verified ||
        refreshed.membership_checked_at_ms != source.membership_checked_at_ms ||
        refreshed.last_refresh_at_ms != source.last_refresh_at_ms;
    if (changed)
        SaveSession(refreshed);
    return refreshed;
}

AuthSession GfnClient::ForceRefreshSavedSession(const AuthSession& session) const
{
    if (!session.persistence_enabled)
    {
        AuthSession refreshed = session;
        refreshed.tokens = RefreshSessionTokens(http_client_, session);
        if (refreshed.tokens.client_token.empty() ||
            IsNearExpiry(refreshed.tokens.client_token_expires_at_ms))
        {
            RequestClientToken(http_client_, refreshed.tokens);
        }
        refreshed.last_refresh_at_ms = NowMs();
        RefreshMembershipTier(http_client_, refreshed);
        return refreshed;
    }

    std::lock_guard<std::recursive_mutex> lock(AccountsMutex());
    AuthSession source = session;
    for (const AuthSession& saved : LoadAccountsFromDisk(nullptr))
    {
        if (saved.user.user_id == session.user.user_id &&
            (saved.last_refresh_at_ms > source.last_refresh_at_ms ||
             saved.tokens.expires_at_ms > source.tokens.expires_at_ms))
        {
            source = saved;
        }
    }

    AppendAuthLog("auth: forced refresh after authorization failure user_id_present=" +
                  std::string(source.user.user_id.empty() ? "no" : "yes"));
    AuthSession refreshed = source;
    refreshed.tokens = RefreshSessionTokens(http_client_, source);
    if (refreshed.tokens.client_token.empty() ||
        IsNearExpiry(refreshed.tokens.client_token_expires_at_ms))
    {
        RequestClientToken(http_client_, refreshed.tokens);
    }
    refreshed.last_refresh_at_ms = NowMs();
    RefreshMembershipTier(http_client_, refreshed);
    SaveSession(refreshed);
    return refreshed;
}

} // namespace opennow
