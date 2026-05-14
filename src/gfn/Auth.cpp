#include "opennow/gfn/Auth.hpp"

#include <array>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>
#include <vector>

#include "opennow/util/Json.hpp"

namespace opennow::gfn {

namespace {
constexpr const char* kClientId = "ZU7sPN-miLujMD95LfOQ453IB0AtjM8sMyvgJ9wCXEQ";
constexpr const char* kSteamDeckClientId = "q61ddeJrVt7O90Nl-P-N7I36yctih4Ml6FyXLrb6j-U";
constexpr const char* kScopes = "openid consent email tk_client age";
constexpr const char* kDefaultIdpId = "PDiAhv2kJTFeQ7WOPqiQ2tRZ7lGhR2X11dXvM4TZSxg";
constexpr const char* kGfnUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36 "
    "NVIDIACEFClient/HEAD/debb5919f6 GFN-PC/2.0.80.173";
constexpr const char* kSteamDeckUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64; Steam Deck) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36";

struct ClientProfileConfig {
    const char* name;
    const char* clientId;
    const char* scopes;
    const char* userAgent;
    const char* origin;
    const char* referer;
    bool steamDeckHeaders;
};

const ClientProfileConfig& clientProfile(AuthClientProfile profile) {
    static constexpr ClientProfileConfig desktop{
        "Desktop",
        kClientId,
        kScopes,
        kGfnUserAgent,
        "https://nvfile",
        "",
        false,
    };
    static constexpr ClientProfileConfig steamDeck{
        "Steam Deck",
        kSteamDeckClientId,
        kScopes,
        kSteamDeckUserAgent,
        "https://play.geforcenow.com",
        "https://play.geforcenow.com/",
        true,
    };
    return profile == AuthClientProfile::SteamDeck ? steamDeck : desktop;
}

std::string formPair(const std::string& key, const std::string& value) {
    return urlEncode(key) + "=" + urlEncode(value);
}

std::array<std::uint8_t, 32> sha256(std::string_view input) {
    constexpr std::array<std::uint32_t, 64> k{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };

    auto rotr = [](std::uint32_t value, std::uint32_t shift) {
        return (value >> shift) | (value << (32U - shift));
    };

    std::vector<std::uint8_t> data(input.begin(), input.end());
    const auto bitLength = static_cast<std::uint64_t>(data.size()) * 8ULL;
    data.push_back(0x80U);
    while ((data.size() % 64U) != 56U) {
        data.push_back(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.push_back(static_cast<std::uint8_t>((bitLength >> shift) & 0xffU));
    }

    std::uint32_t h0 = 0x6a09e667U;
    std::uint32_t h1 = 0xbb67ae85U;
    std::uint32_t h2 = 0x3c6ef372U;
    std::uint32_t h3 = 0xa54ff53aU;
    std::uint32_t h4 = 0x510e527fU;
    std::uint32_t h5 = 0x9b05688cU;
    std::uint32_t h6 = 0x1f83d9abU;
    std::uint32_t h7 = 0x5be0cd19U;

    for (std::size_t offset = 0; offset < data.size(); offset += 64U) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16U; ++i) {
            const auto j = offset + i * 4U;
            w[i] = (static_cast<std::uint32_t>(data[j]) << 24U)
                | (static_cast<std::uint32_t>(data[j + 1U]) << 16U)
                | (static_cast<std::uint32_t>(data[j + 2U]) << 8U)
                | static_cast<std::uint32_t>(data[j + 3U]);
        }
        for (std::size_t i = 16U; i < 64U; ++i) {
            const auto s0 = rotr(w[i - 15U], 7U) ^ rotr(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
            const auto s1 = rotr(w[i - 2U], 17U) ^ rotr(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
            w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
        }

        auto a = h0;
        auto b = h1;
        auto c = h2;
        auto d = h3;
        auto e = h4;
        auto f = h5;
        auto g = h6;
        auto h = h7;

        for (std::size_t i = 0; i < 64U; ++i) {
            const auto s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
            const auto ch = (e & f) ^ ((~e) & g);
            const auto temp1 = h + s1 + ch + k[i] + w[i];
            const auto s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    }

    const std::array<std::uint32_t, 8> words{h0, h1, h2, h3, h4, h5, h6, h7};
    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < words.size(); ++i) {
        digest[i * 4U] = static_cast<std::uint8_t>((words[i] >> 24U) & 0xffU);
        digest[i * 4U + 1U] = static_cast<std::uint8_t>((words[i] >> 16U) & 0xffU);
        digest[i * 4U + 2U] = static_cast<std::uint8_t>((words[i] >> 8U) & 0xffU);
        digest[i * 4U + 3U] = static_cast<std::uint8_t>(words[i] & 0xffU);
    }
    return digest;
}

std::vector<std::uint8_t> randomBytes(std::size_t count) {
    std::vector<std::uint8_t> bytes(count);
    std::random_device rd;
    std::mt19937_64 rng(rd());
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(rng() & 0xffU);
    }
    return bytes;
}
}

AuthRequestBuilder::AuthRequestBuilder(AuthEndpoints endpoints, AuthClientProfile profile)
    : endpoints_(std::move(endpoints)), profile_(profile) {}

AuthRequestBuilder AuthRequestBuilder::steamDeck(AuthEndpoints endpoints) {
    return AuthRequestBuilder{std::move(endpoints), AuthClientProfile::SteamDeck};
}

LoginProvider AuthRequestBuilder::defaultProvider() const {
    return {
        kDefaultIdpId,
        "NVIDIA",
        "NVIDIA",
        "https://prod.cloudmatchbeta.nvidiagrid.net/",
        0,
    };
}

LoginProvider AuthRequestBuilder::normalizeProvider(LoginProvider provider) const {
    if (!provider.streamingServiceUrl.empty() && provider.streamingServiceUrl.back() != '/') {
        provider.streamingServiceUrl.push_back('/');
    }
    return provider;
}

const char* AuthRequestBuilder::profileName() const {
    return clientProfile(profile_).name;
}

std::string AuthRequestBuilder::buildAuthUrl(const AuthUrlRequest& request) const {
    const auto provider = normalizeProvider(request.provider);
    const auto redirectUri = "http://localhost:" + std::to_string(request.redirectPort);
    const auto& profile = clientProfile(profile_);

    std::vector<std::string> pairs;
    pairs.push_back(formPair("response_type", "code"));
    pairs.push_back(formPair("device_id", request.deviceId));
    pairs.push_back(formPair("scope", profile.scopes));
    pairs.push_back(formPair("client_id", profile.clientId));
    pairs.push_back(formPair("redirect_uri", redirectUri));
    pairs.push_back(formPair("ui_locales", "en_US"));
    pairs.push_back(formPair("nonce", request.nonce));
    pairs.push_back(formPair("prompt", "select_account"));
    pairs.push_back(formPair("code_challenge", request.challenge));
    pairs.push_back(formPair("code_challenge_method", "S256"));
    pairs.push_back(formPair("idp_id", provider.idpId));

    std::ostringstream url;
    url << endpoints_.authorize << "?";
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        if (i != 0) {
            url << "&";
        }
        url << pairs[i];
    }
    return url.str();
}

std::string AuthRequestBuilder::buildTokenExchangeBody(const std::string& code, const std::string& verifier, std::uint16_t redirectPort) const {
    const auto redirectUri = "http://localhost:" + std::to_string(redirectPort);
    return formPair("grant_type", "authorization_code")
        + "&" + formPair("code", code)
        + "&" + formPair("redirect_uri", redirectUri)
        + "&" + formPair("code_verifier", verifier);
}

std::string AuthRequestBuilder::buildRefreshTokenBody(const std::string& refreshToken) const {
    const auto& profile = clientProfile(profile_);
    return formPair("grant_type", "refresh_token")
        + "&" + formPair("refresh_token", refreshToken)
        + "&" + formPair("client_id", profile.clientId);
}

std::string AuthRequestBuilder::buildClientTokenRefreshBody(const std::string& clientToken, const std::string& userId) const {
    const auto& profile = clientProfile(profile_);
    return formPair("grant_type", "urn:ietf:params:oauth:grant-type:client_token")
        + "&" + formPair("client_token", clientToken)
        + "&" + formPair("client_id", profile.clientId)
        + "&" + formPair("sub", userId);
}

std::string AuthRequestBuilder::buildDeviceAuthorizeBody(const DeviceAuthorizationRequest& request) const {
    const auto& profile = clientProfile(profile_);
    auto body = formPair("client_id", profile.clientId)
        + "&" + formPair("scope", profile.scopes)
        + "&" + formPair("device_id", request.deviceId)
        + "&" + formPair("display_name", request.displayName.empty() ? "OpenNOW Switch" : request.displayName);
    if (!request.idpId.empty()) {
        body += "&" + formPair("idp_id", request.idpId);
    }
    return body;
}

std::string AuthRequestBuilder::buildDeviceCodeTokenBody(const std::string& deviceCode) const {
    const auto& profile = clientProfile(profile_);
    return formPair("grant_type", "urn:ietf:params:oauth:grant-type:device_code")
        + "&" + formPair("device_code", deviceCode)
        + "&" + formPair("client_id", profile.clientId);
}

net::HttpRequest AuthRequestBuilder::buildServiceUrlsRequest() const {
    return {net::HttpMethod::Get, endpoints_.serviceUrls, authHeaders(false), ""};
}

net::HttpRequest AuthRequestBuilder::buildTokenExchangeRequest(const std::string& code, const std::string& verifier, std::uint16_t redirectPort) const {
    return {net::HttpMethod::Post, endpoints_.token, authHeaders(true), buildTokenExchangeBody(code, verifier, redirectPort)};
}

net::HttpRequest AuthRequestBuilder::buildRefreshTokenRequest(const std::string& refreshToken) const {
    return {net::HttpMethod::Post, endpoints_.token, authHeaders(true), buildRefreshTokenBody(refreshToken)};
}

net::HttpRequest AuthRequestBuilder::buildClientTokenRefreshRequest(const std::string& clientToken, const std::string& userId) const {
    return {net::HttpMethod::Post, endpoints_.token, authHeaders(true), buildClientTokenRefreshBody(clientToken, userId)};
}

net::HttpRequest AuthRequestBuilder::buildDeviceAuthorizeRequest(const DeviceAuthorizationRequest& request) const {
    auto headers = authHeaders(true);
    const auto& profile = clientProfile(profile_);
    if (profile.steamDeckHeaders) {
        headers["x-device-id"] = request.deviceId;
        headers["nv-client-id"] = profile.clientId;
        headers["nv-client-streamer"] = "WEBRTC";
        headers["nv-client-type"] = "BROWSER";
        headers["nv-client-platform-name"] = "browser";
        headers["nv-browser-type"] = "CHROME";
        headers["nv-device-os"] = "STEAMOS";
        headers["nv-device-type"] = "CONSOLE";
        headers["nv-device-model"] = "STEAMDECK";
        headers["nv-device-make"] = "VALVE";
    }
    return {net::HttpMethod::Post, endpoints_.deviceAuthorize, headers, buildDeviceAuthorizeBody(request)};
}

net::HttpRequest AuthRequestBuilder::buildDeviceCodeTokenRequest(const std::string& deviceCode) const {
    return {net::HttpMethod::Post, endpoints_.token, authHeaders(true), buildDeviceCodeTokenBody(deviceCode)};
}

net::HttpRequest AuthRequestBuilder::buildUserInfoRequest(const std::string& accessToken) const {
    auto headers = authHeaders(false);
    headers["Authorization"] = "Bearer " + accessToken;
    headers["Accept"] = "application/json";
    return {net::HttpMethod::Get, endpoints_.userInfo, headers, ""};
}

net::HttpRequest AuthRequestBuilder::buildClientTokenRequest(const std::string& accessToken) const {
    auto headers = authHeaders(false);
    headers["Authorization"] = "Bearer " + accessToken;
    headers["Accept"] = "application/json, text/plain, */*";
    return {net::HttpMethod::Get, endpoints_.clientToken, headers, ""};
}

net::Headers AuthRequestBuilder::authHeaders(bool includeContentType) const {
    const auto& profile = clientProfile(profile_);
    net::Headers headers{
        {"Accept", "application/json, text/plain, */*"},
        {"Origin", profile.origin},
        {"User-Agent", profile.userAgent},
    };
    if (profile.referer[0] != '\0') {
        headers["Referer"] = profile.referer;
    }
    if (includeContentType) {
        headers["Content-Type"] = "application/x-www-form-urlencoded; charset=UTF-8";
    }
    return headers;
}

std::string urlEncode(std::string_view value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << static_cast<char>(c);
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
            encoded << std::setfill(' ');
        }
    }
    return encoded.str();
}

std::string base64UrlEncode(const std::uint8_t* data, std::size_t size) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((size + 2U) / 3U) * 4U);

    for (std::size_t i = 0; i < size; i += 3U) {
        const auto remaining = size - i;
        const auto b0 = data[i];
        const auto b1 = remaining > 1U ? data[i + 1U] : 0U;
        const auto b2 = remaining > 2U ? data[i + 2U] : 0U;
        const auto triple = (static_cast<std::uint32_t>(b0) << 16U)
            | (static_cast<std::uint32_t>(b1) << 8U)
            | static_cast<std::uint32_t>(b2);

        out.push_back(alphabet[(triple >> 18U) & 0x3fU]);
        out.push_back(alphabet[(triple >> 12U) & 0x3fU]);
        if (remaining > 1U) {
            out.push_back(alphabet[(triple >> 6U) & 0x3fU]);
        }
        if (remaining > 2U) {
            out.push_back(alphabet[triple & 0x3fU]);
        }
    }
    return out;
}

std::string generateCodeVerifier(std::size_t byteCount) {
    const auto bytes = randomBytes(byteCount);
    return base64UrlEncode(bytes.data(), bytes.size());
}

std::string buildCodeChallenge(std::string_view verifier) {
    const auto digest = sha256(verifier);
    return base64UrlEncode(digest.data(), digest.size());
}

std::string generateOpaqueToken(std::size_t byteCount) {
    const auto bytes = randomBytes(byteCount);
    return base64UrlEncode(bytes.data(), bytes.size());
}

DeviceAuthorizationInfo parseDeviceAuthorizationResponse(const std::string& body) {
    DeviceAuthorizationInfo info;
    const auto parsed = util::parseJson(body);
    if (!parsed.ok) {
        return info;
    }

    if (const auto* value = parsed.value.get("device_code")) info.deviceCode = value->asString();
    if (const auto* value = parsed.value.get("user_code")) info.userCode = value->asString();
    if (const auto* value = parsed.value.get("verification_uri")) info.verificationUri = value->asString();
    if (const auto* value = parsed.value.get("verification_uri_complete")) info.verificationUriComplete = value->asString();
    if (const auto* value = parsed.value.get("expires_in")) info.expiresIn = static_cast<std::uint32_t>(value->asNumber());
    if (const auto* value = parsed.value.get("interval")) info.interval = static_cast<std::uint32_t>(value->asNumber(5));
    if (info.interval == 0) {
        info.interval = 5;
    }
    return info;
}

} // namespace opennow::gfn
