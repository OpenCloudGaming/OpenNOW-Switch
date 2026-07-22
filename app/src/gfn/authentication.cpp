#include "internal.hpp"
#include "persistence_internal.hpp"

#include "../auth_policy.hpp"
#include "../native_auth_policy.hpp"
#include "../network_utils.hpp"

#include <curl/curl.h>

#ifdef __SWITCH__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <switch.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
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
constexpr const char* kClientTokenEndpoint = "https://login.nvidia.com/client_token";
constexpr const char* kUserInfoEndpoint    = "https://login.nvidia.com/userinfo";
constexpr const char* kAuthorizeEndpoint   = "https://login.nvidia.com/authorize";
constexpr const char* kSubscriptionEndpoint = "https://mes.geforcenow.com/v4/subscriptions";
constexpr const char* kClientId            = "ZU7sPN-miLujMD95LfOQ453IB0AtjM8sMyvgJ9wCXEQ";
constexpr const char* kLcarsClientId       = "ec7e38d4-03af-4b58-b131-cfb0495903ab";
constexpr const char* kGfnClientVersion    = "2.0.80.173";
constexpr const char* kScopes              = "openid consent email tk_client age";
constexpr const char* kRedirectUri         = "http://localhost:2259";
constexpr int kLoginCallbackPort           = 2259;
constexpr const char* kNvidiaFileOrigin    = "https://nvfile";
constexpr const char* kNvidiaFileReferer   = "https://nvfile/";
constexpr std::int64_t kRefreshWindowMs    = 10LL * 60LL * 1000LL;
constexpr std::int64_t kMembershipRefreshIntervalMs = 60LL * 60LL * 1000LL;
constexpr auto kQrLoginTimeout             = std::chrono::minutes(5);
std::mutex g_reauthentication_mutex;

std::string Base64UrlEncode(const unsigned char* data, size_t length)
{
    static constexpr char kBase64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string output;
    output.reserve(((length + 2) / 3) * 4);

    for (size_t index = 0; index < length; index += 3)
    {
        const unsigned int octet_a = data[index];
        const unsigned int octet_b = index + 1 < length ? data[index + 1] : 0;
        const unsigned int octet_c = index + 2 < length ? data[index + 2] : 0;

        const unsigned int triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        output.push_back(kBase64[(triple >> 18) & 0x3F]);
        output.push_back(kBase64[(triple >> 12) & 0x3F]);
        output.push_back(index + 1 < length ? kBase64[(triple >> 6) & 0x3F] : '=');
        output.push_back(index + 2 < length ? kBase64[triple & 0x3F] : '=');
    }

    while (!output.empty() && output.back() == '=')
        output.pop_back();

    std::replace(output.begin(), output.end(), '+', '-');
    std::replace(output.begin(), output.end(), '/', '_');
    return output;
}
#ifdef __SWITCH__
std::string FormatResult(Result rc)
{
    char buffer[32] {};
    std::snprintf(buffer, sizeof(buffer), "0x%08x", static_cast<unsigned int>(rc));
    return std::string(buffer);
}
#endif

struct PkcePair
{
    std::string verifier;
    std::string challenge;
};

PkcePair GeneratePkce()
{
    const auto verifier_bytes = GenerateRandomBytes(64);
    PkcePair result;
    result.verifier = Base64UrlEncode(verifier_bytes.data(), verifier_bytes.size());
    if (result.verifier.size() > 86)
        result.verifier.resize(86);

    std::array<unsigned char, 32> hash {};
#ifdef __SWITCH__
    sha256CalculateHash(hash.data(), result.verifier.data(), result.verifier.size());
#else
    throw std::runtime_error("PKCE generation is only supported in the Switch build");
#endif
    result.challenge = Base64UrlEncode(hash.data(), hash.size());
    return result;
}

std::string UrlDecode(const std::string& input)
{
    std::string output;
    output.reserve(input.size());

    for (size_t index = 0; index < input.size(); ++index)
    {
        if (input[index] == '+' )
        {
            output.push_back(' ');
            continue;
        }

        if (input[index] == '%' && index + 2 < input.size())
        {
            const auto hex = input.substr(index + 1, 2);
            char* end      = nullptr;
            const long value = std::strtol(hex.c_str(), &end, 16);
            if (end && *end == '\0')
            {
                output.push_back(static_cast<char>(value));
                index += 2;
                continue;
            }
        }

        output.push_back(input[index]);
    }

    return output;
}

std::string GetQueryValue(const std::string& url, const std::string& key)
{
    const auto query_pos = url.find('?');
    if (query_pos == std::string::npos)
        return "";

    size_t current = query_pos + 1;
    while (current < url.size())
    {
        const auto next      = url.find('&', current);
        const std::string kv = url.substr(current, next == std::string::npos ? std::string::npos : next - current);
        const auto equals    = kv.find('=');
        const std::string raw_key =
            equals == std::string::npos ? kv : kv.substr(0, equals);

        if (UrlDecode(raw_key) == key)
        {
            const std::string raw_value =
                equals == std::string::npos ? std::string() : kv.substr(equals + 1);
            return UrlDecode(raw_value);
        }

        if (next == std::string::npos)
            break;

        current = next + 1;
    }

    return "";
}

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

struct NativeAuthResponse
{
    long status_code = 0;
    std::string body;
    std::string location;
};

class NativeAuthHttpSession
{
  public:
    NativeAuthHttpSession()
        : curl_(curl_easy_init(), &curl_easy_cleanup)
    {
        if (!curl_)
            throw std::runtime_error("Unable to initialize the native NVIDIA login connection");
    }

    NativeAuthResponse Request(
        const std::string& method,
        const std::string& url,
        const std::vector<std::string>& headers = {},
        const std::string& body = {})
    {
        curl_easy_reset(curl_.get());
        response_body_.clear();
        response_location_.clear();

        curl_slist* raw_headers = nullptr;
        for (const auto& header : headers)
            raw_headers = curl_slist_append(raw_headers, header.c_str());
        std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_list(
            raw_headers, &curl_slist_free_all);

        curl_easy_setopt(curl_.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_.get(), CURLOPT_USERAGENT, GfnClient::kUserAgent);
        curl_easy_setopt(curl_.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl_.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_.get(), CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl_.get(), CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl_.get(), CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl_.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl_.get(), CURLOPT_COOKIEFILE, "");
        curl_easy_setopt(curl_.get(), CURLOPT_WRITEFUNCTION, &NativeAuthHttpSession::WriteBody);
        curl_easy_setopt(curl_.get(), CURLOPT_WRITEDATA, this);
        curl_easy_setopt(curl_.get(), CURLOPT_HEADERFUNCTION, &NativeAuthHttpSession::WriteHeader);
        curl_easy_setopt(curl_.get(), CURLOPT_HEADERDATA, this);
        if (header_list)
            curl_easy_setopt(curl_.get(), CURLOPT_HTTPHEADER, header_list.get());

        if (method == "POST")
        {
            curl_easy_setopt(curl_.get(), CURLOPT_POST, 1L);
            curl_easy_setopt(curl_.get(), CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl_.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
        else
        {
            curl_easy_setopt(curl_.get(), CURLOPT_HTTPGET, 1L);
        }

        const CURLcode rc = curl_easy_perform(curl_.get());
        if (rc != CURLE_OK)
            throw std::runtime_error(
                std::string("Native NVIDIA login network error: ") + curl_easy_strerror(rc));

        long status_code = 0;
        curl_easy_getinfo(curl_.get(), CURLINFO_RESPONSE_CODE, &status_code);
        return {status_code, response_body_, response_location_};
    }

  private:
    static size_t WriteBody(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        auto* self = static_cast<NativeAuthHttpSession*>(userdata);
        const size_t bytes = size * nmemb;
        self->response_body_.append(ptr, bytes);
        return bytes;
    }

    static size_t WriteHeader(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        auto* self = static_cast<NativeAuthHttpSession*>(userdata);
        const size_t bytes = size * nmemb;
        std::string line(ptr, bytes);
        if (line.size() >= 9)
        {
            std::string prefix = line.substr(0, 9);
            std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (prefix == "location:")
                self->response_location_ = Trim(line.substr(9));
        }
        return bytes;
    }

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl_;
    std::string response_body_;
    std::string response_location_;
};

std::string ResolveRedirectUrl(const std::string& current_url, const std::string& location)
{
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0)
        return location;
    if (location.empty() || location.front() != '/')
        throw std::runtime_error("NVIDIA login returned an unsupported redirect");

    const auto scheme_end = current_url.find("://");
    const auto host_end = scheme_end == std::string::npos
        ? std::string::npos
        : current_url.find('/', scheme_end + 3);
    if (scheme_end == std::string::npos)
        throw std::runtime_error("NVIDIA login returned an invalid redirect origin");
    return current_url.substr(0, host_end) + location;
}

std::vector<std::string> NativeAuthHeaders(const std::string& key, bool metrics = false)
{
    std::vector<std::string> headers {
        "Accept: application/json",
        "Content-Type: application/json",
        "Accept-Language: en-US",
        "Authorization: Bearer " + key,
        "Origin: https://login.nvgs.nvidia.com",
        "Referer: https://login.nvgs.nvidia.com/",
    };
    if (metrics)
    {
        headers.push_back(
            "x-metrics: 5ZjiARedWmKOoXlrQRFkblcGoDlJGPOT0oHz4C03hOS72FpYLX0LRmdNk8GbhBIJ");
    }
    return headers;
}

std::string NativeErrorCode(const std::string& body)
{
    try
    {
        JsonPtr root = LoadJson(body);
        std::string error = GetString(root.get(), "error");
        if (error.empty())
            error = GetString(root.get(), "error_description");
        return error;
    }
    catch (...)
    {
        return {};
    }
}

JsonPtr NativeJsonRequest(
    NativeAuthHttpSession& session,
    const std::string& method,
    const std::string& url,
    const std::string& key,
    json_t* payload,
    bool metrics = false)
{
    const NativeAuthResponse response = session.Request(
        method, url, NativeAuthHeaders(key, metrics), payload ? DumpJson(payload) : std::string {});
    if (response.status_code < 200 || response.status_code >= 300)
    {
        const std::string code = NativeErrorCode(response.body);
        AppendAuthLog(
            "auth-native: request failed HTTP=" + std::to_string(response.status_code) +
            " error=" + (code.empty() ? "unknown" : code));
        if (code == "VALIDATION_REQUIRED" || code == "CAPTCHA_REQUIRED")
            throw NativeLoginFallbackRequired("NVIDIA requires a CAPTCHA for this sign-in");
        if (code == "CREDENTIALS_INVALID" || code == "UNAUTHORIZED")
            throw std::runtime_error("Incorrect NVIDIA email, password, or verification code");
        if (code == "ITEM_NOT_FOUND")
            throw std::runtime_error("No NVIDIA account was found for this email");
        throw std::runtime_error(
            "NVIDIA native login failed (HTTP " + std::to_string(response.status_code) +
            (code.empty() ? ")" : ", " + code + ")"));
    }
    return LoadJson(response.body);
}

struct NativeStage
{
    std::string page;
    std::string key;
    std::string external_url;
    JsonPtr payload {nullptr, &json_decref};
};

NativeStage AdvanceNativeStage(NativeAuthHttpSession& session, const std::string& key)
{
    JsonPtr empty(json_object(), &json_decref);
    JsonPtr root = NativeJsonRequest(
        session, "POST", "https://accounts.nvgs.nvidia.com/api/1/frontend/oauth/user/next",
        key, empty.get());
    NativeStage stage;
    stage.page = GetString(root.get(), "page");
    stage.key = GetString(root.get(), "key");
    stage.external_url = GetString(root.get(), "externalUrl");
    stage.payload = std::move(root);
    if (stage.key.empty())
        stage.key = key;
    AppendAuthLog("auth-native: stage=" + (stage.page.empty() ? "unknown" : stage.page));
    return stage;
}

std::string BuildAuthorizeUrl(const LoginProvider& provider, const PkcePair& pkce,
                              const std::string& state,
                              const std::string& redirect_uri = std::string(kRedirectUri),
                              const std::string& login_hint = {})
{
    const auto nonce = HexEncode(GenerateRandomBytes(16).data(), 16);

    std::string url = std::string(kAuthorizeEndpoint) + "?";
    url += "response_type=code";
    url += "&device_id=" + UrlEncode(GenerateDeviceId());
    url += "&scope=" + UrlEncode(kScopes, true);
    url += "&client_id=" + UrlEncode(kClientId);
    url += "&redirect_uri=" + UrlEncode(redirect_uri);
    url += "&ui_locales=" + UrlEncode("en_US");
    url += "&nonce=" + UrlEncode(nonce);
    url += "&state=" + UrlEncode(state);
    url += "&prompt=" + UrlEncode(login_hint.empty() ? "select_account" : "login");
    if (!login_hint.empty())
        url += "&login_hint=" + UrlEncode(login_hint);
    url += "&code_challenge=" + UrlEncode(pkce.challenge);
    url += "&code_challenge_method=S256";
    url += "&idp_id=" + UrlEncode(provider.idp_id);
    return url;
}

#ifdef __SWITCH__
class OAuthCallbackServer
{
  public:
    explicit OAuthCallbackServer(WebCommonConfig* browser_config)
        : browser_config_(browser_config)
    {
    }

    ~OAuthCallbackServer()
    {
        Stop();
    }

    void Start()
    {
        worker_ = std::thread([this]() {
            Run();
        });
    }

    void Stop()
    {
        stop_ = true;
        if (worker_.joinable())
            worker_.join();
    }

    std::string WaitForResult()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!condition_.wait_for(lock, std::chrono::seconds(8), [this]() {
                return completed_;
            }))
        {
            return "";
        }

        if (!error_.empty())
            throw std::runtime_error(error_);

        return callback_url_;
    }

  private:
    void Complete(std::string callback_url, std::string error = {})
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (completed_)
                return;

            callback_url_ = std::move(callback_url);
            error_        = std::move(error);
            completed_    = true;
        }

        condition_.notify_all();

        if (browser_config_)
            webConfigRequestExit(browser_config_);
    }

    void SendResponse(int client, bool ok)
    {
        const std::string body =
            ok
                ? "<!doctype html><html><body style=\"font-family:sans-serif;background:#101418;color:#eef;padding:32px\"><h2>SwitchNOW login complete</h2><p>You can return to SwitchNOW now.</p></body></html>"
                : "<!doctype html><html><body style=\"font-family:sans-serif;background:#101418;color:#eef;padding:32px\"><h2>SwitchNOW login failed</h2><p>Return to SwitchNOW and check auth.log.</p></body></html>";

        const std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Connection: close\r\n"
            "Content-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;

        send(client, response.data(), response.size(), 0);
    }

    void Run()
    {
        const int server = socket(AF_INET, SOCK_STREAM, 0);
        if (server < 0)
        {
            Complete({}, "OAuth callback server socket() failed");
            return;
        }

        int reuse = 1;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address {};
        address.sin_family      = AF_INET;
        address.sin_port        = htons(kLoginCallbackPort);
        address.sin_addr.s_addr = inet_addr("127.0.0.1");

        if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
        {
            close(server);
            Complete({}, std::string("OAuth callback server bind() failed on localhost:") + std::to_string(kLoginCallbackPort));
            return;
        }

        if (listen(server, 1) < 0)
        {
            close(server);
            Complete({}, "OAuth callback server listen() failed");
            return;
        }

        AppendAuthLog(std::string("auth: callback server listening on localhost:") + std::to_string(kLoginCallbackPort));

        while (!stop_)
        {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(server, &read_fds);

            timeval timeout {};
            timeout.tv_sec = 1;

            const int ready = select(server + 1, &read_fds, nullptr, nullptr, &timeout);
            if (ready <= 0)
                continue;

            const int client = accept(server, nullptr, nullptr);
            if (client < 0)
                continue;

            char buffer[2048] {};
            const ssize_t read = recv(client, buffer, sizeof(buffer) - 1, 0);
            if (read <= 0)
            {
                close(client);
                continue;
            }

            const std::string request(buffer, static_cast<size_t>(read));
            const auto first_space  = request.find(' ');
            const auto second_space = first_space == std::string::npos
                                          ? std::string::npos
                                          : request.find(' ', first_space + 1);

            std::string target =
                first_space == std::string::npos || second_space == std::string::npos
                    ? std::string("/")
                    : request.substr(first_space + 1, second_space - first_space - 1);

            if (target.rfind("http://", 0) != 0)
                target = std::string(kRedirectUri) + target;

            const bool ok = !GetQueryValue(target, "code").empty() || !GetQueryValue(target, "error").empty();
            SendResponse(client, ok);
            close(client);

            AppendAuthLog(
                "auth: callback server received code=" +
                std::string(GetQueryValue(target, "code").empty() ? "no" : "yes") +
                " error=" +
                std::string(GetQueryValue(target, "error").empty() ? "no" : "yes"));

            if (ok)
            {
                Complete(target);
                break;
            }
        }

        close(server);
    }

    WebCommonConfig* browser_config_ = nullptr;
    std::atomic<bool> stop_ {false};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool completed_ = false;
    std::string callback_url_;
    std::string error_;
};

std::string ExtractCallbackUrlFromReply(WebCommonReply* reply)
{
    char last_url[0x1000] {};
    size_t last_url_size = 0;

    Result rc = webReplyGetLastUrl(reply, last_url, sizeof(last_url), &last_url_size);
    if (R_SUCCEEDED(rc) && last_url[0] != '\0')
        return std::string(last_url);

    if (!reply->type && reply->ret.lastUrl[0] != '\0')
        return std::string(reply->ret.lastUrl);

    return "";
}
#endif

std::string RunBrowserLogin(const std::string& auth_url)
{
#ifdef __SWITCH__
    const AppletType applet_type = appletGetAppletType();
    if (applet_type != AppletType_Application && applet_type != AppletType_SystemApplication)
    {
        AppendAuthLog("auth: refused browser login outside application mode");
        throw std::runtime_error(
            "NVIDIA login needs Switch application mode. Launch Homebrew Menu with title override: hold R while opening any installed game, then start SwitchNOW from there.");
    }

    AppendAuthLog("auth: creating browser applet");

    WebCommonConfig config {};
    Result rc = webPageCreate(&config, auth_url.c_str());
    if (R_FAILED(rc))
    {
        AppendAuthLog("auth: webPageCreate failed " + FormatResult(rc));
        throw std::runtime_error("Unable to open the Switch browser applet: " + FormatResult(rc));
    }

    rc = webConfigSetWhitelist(&config, "^https?://.*");
    if (R_FAILED(rc))
    {
        AppendAuthLog("auth: webConfigSetWhitelist failed " + FormatResult(rc));
        throw std::runtime_error("Failed to configure the browser whitelist: " + FormatResult(rc));
    }

    const Result callback_rc = webConfigSetCallbackUrl(&config, kRedirectUri);
    const bool system_callback = R_SUCCEEDED(callback_rc);
    AppendAuthLog(
        "auth: system callback mode=" + std::string(system_callback ? "enabled" : "unavailable") +
        " rc=" + FormatResult(callback_rc));

    std::unique_ptr<OAuthCallbackServer> callback_server;
    if (!system_callback)
    {
        callback_server = std::make_unique<OAuthCallbackServer>(&config);
        callback_server->Start();
    }

    WebCommonReply reply {};
    AppendAuthLog("auth: showing browser applet");
    rc = webConfigShow(&config, &reply);
    if (R_FAILED(rc))
    {
        AppendAuthLog("auth: webConfigShow failed " + FormatResult(rc));
        throw std::runtime_error("The Switch browser closed before login completed: " + FormatResult(rc));
    }

    std::string callback_url = ExtractCallbackUrlFromReply(&reply);
    if (callback_url.empty() && callback_server)
        callback_url = callback_server->WaitForResult();
    if (callback_server)
        callback_server->Stop();

    if (!callback_url.empty())
        return callback_url;

    WebExitReason exit_reason = WebExitReason_UnknownE;
    rc                        = webReplyGetExitReason(&reply, &exit_reason);
    if (R_SUCCEEDED(rc))
        AppendAuthLog("auth: browser exit reason without callback " + std::to_string(static_cast<int>(exit_reason)));

    throw std::runtime_error("GeForce NOW login browser closed before the OAuth callback was received");
#else
    (void)auth_url;
    throw std::runtime_error("Browser login is only supported in the Switch build");
#endif
}

AuthTokens ParseAuthTokens(json_t* payload)
{
    AuthTokens tokens;
    tokens.access_token              = GetString(payload, "access_token");
    tokens.refresh_token             = GetString(payload, "refresh_token");
    tokens.id_token                  = GetString(payload, "id_token");
    tokens.client_token              = GetString(payload, "client_token");
    tokens.expires_at_ms             = ToExpiresAtMs(payload, "expires_in");
    tokens.client_token_expires_at_ms = ToExpiresAtMs(payload, "expires_in");

    if (tokens.access_token.empty())
        throw std::runtime_error("Login succeeded but no access token was returned");

    return tokens;
}

AuthTokens ExchangeAuthorizationCode(
    const HttpClient& http_client,
    const std::string& code,
    const std::string& verifier,
    const std::string& redirect_uri = std::string(kRedirectUri))
{
    const std::string body =
        "grant_type=authorization_code"
        "&code=" + UrlEncode(code, true) +
        "&redirect_uri=" + UrlEncode(redirect_uri, true) +
        "&code_verifier=" + UrlEncode(verifier, true);

    const HttpResponse response = http_client.Post(
        kTokenEndpoint,
        GfnClient::kUserAgent,
        BuildNvidiaAuthHeaders({}, "application/x-www-form-urlencoded; charset=UTF-8", true),
        body);

    if (response.status_code != 200)
    {
        AppendAuthLog(
            "auth: token exchange failed HTTP " + std::to_string(response.status_code) +
            " body=" + JsonForTrace(response.body).substr(0, 240));
        throw std::runtime_error(
            "Token exchange failed with HTTP " + std::to_string(response.status_code));
    }

    JsonPtr root = LoadJson(response.body);
    return ParseAuthTokens(root.get());
}

AuthTokens RefreshTokens(const HttpClient& http_client, const AuthSession& session)
{
    if (session.tokens.refresh_token.empty())
        throw ReauthenticationRequired("Saved GeForce NOW session cannot be refreshed. Please sign in again.");

    const std::string body =
        "grant_type=refresh_token"
        "&refresh_token=" + UrlEncode(session.tokens.refresh_token, true) +
        "&client_id=" + UrlEncode(kClientId, true);

    HttpResponse response;
    for (int attempt = 1; attempt <= 3; ++attempt)
    {
        response = http_client.Post(
            kTokenEndpoint,
            GfnClient::kUserAgent,
            BuildNvidiaAuthHeaders({}, "application/x-www-form-urlencoded; charset=UTF-8"),
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
    AuthTokens tokens = ParseAuthTokens(root.get());
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

AuthTokens RefreshTokensWithClientToken(
    const HttpClient& http_client, const AuthSession& session)
{
    if (session.tokens.client_token.empty() || session.user.user_id.empty())
        throw std::runtime_error("Saved session has no client-token refresh mechanism");

    const std::string body =
        "grant_type=" + UrlEncode("urn:ietf:params:oauth:grant-type:client_token", true) +
        "&client_token=" + UrlEncode(session.tokens.client_token, true) +
        "&client_id=" + UrlEncode(kClientId, true) +
        "&sub=" + UrlEncode(session.user.user_id, true);

    const HttpResponse response = http_client.Post(
        kTokenEndpoint,
        GfnClient::kUserAgent,
        BuildNvidiaAuthHeaders({}, "application/x-www-form-urlencoded; charset=UTF-8"),
        body);
    if (response.status_code != 200)
    {
        throw std::runtime_error(
            "Client-token refresh failed with HTTP " + std::to_string(response.status_code));
    }

    JsonPtr root = LoadJson(response.body);
    AuthTokens tokens = ParseAuthTokens(root.get());
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
        GfnClient::kUserAgent,
        BuildNvidiaAuthHeaders(tokens.access_token));

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
        GfnClient::kUserAgent,
        BuildNvidiaAuthHeaders(tokens.access_token, {}, false, "application/json"));

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

std::string FetchVpcId(const HttpClient& http_client, const AuthSession& session)
{
    const std::string token = ResolveSessionJwt(session);
    if (token.empty())
        return "NP-AMS-08";

    const std::string url = EnsureTrailingSlash(session.provider.streaming_service_url) +
        "v2/serverInfo";
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
        const std::string tier = Trim(GetString(root.get(), "membershipTier"));
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

AuthSession GfnClient::Login(const LoginProvider& provider, const std::string& login_hint) const
{
    AppendAuthLog("auth: login started provider=" + provider.code);

    const PkcePair pkce = GeneratePkce();
    const std::string state = HexEncode(GenerateRandomBytes(32).data(), 32);
    const std::string auth_url = BuildAuthorizeUrl(provider, pkce, state, kRedirectUri, login_hint);
    const std::string final_url = RunBrowserLogin(auth_url);

    if (GetQueryValue(final_url, "state") != state)
        throw std::runtime_error("GeForce NOW login callback failed the OAuth state check");

    const std::string error = GetQueryValue(final_url, "error");
    if (!error.empty())
    {
        throw std::runtime_error("GeForce NOW login failed: " + error);
    }

    const std::string code = GetQueryValue(final_url, "code");
    if (code.empty())
        throw std::runtime_error("The GeForce NOW login callback did not include an authorization code");

    AuthTokens tokens = ExchangeAuthorizationCode(http_client_, code, pkce.verifier);
    AppendAuthLog("auth: token exchange ok");
    RequestClientToken(http_client_, tokens);

    AuthSession session;
    session.provider = provider;
    session.tokens   = tokens;
    session.user     = FetchUserInfo(http_client_, tokens);
    session.last_refresh_at_ms = NowMs();
    RefreshMembershipTier(http_client_, session);
    AppendAuthLog("auth: user info ok user_id_present=" + std::string(session.user.user_id.empty() ? "no" : "yes"));
    return session;
}

AuthSession GfnClient::LoginNative(
    const LoginProvider& provider,
    const std::string& email,
    const std::string& password,
    const std::function<std::string(const std::string&)>& request_one_time_code,
    const std::function<void(const std::string&)>& report_status,
    const std::function<bool()>& is_cancelled) const
{
    if (Trim(email).empty() || password.empty())
        throw std::runtime_error("Email and password are required");

    AppendAuthLog("auth-native: login started provider=" + provider.code);
    const PkcePair pkce = GeneratePkce();
    const std::string state = HexEncode(GenerateRandomBytes(32).data(), 32);
    const std::string auth_url = BuildAuthorizeUrl(provider, pkce, state, kRedirectUri, Trim(email));
    NativeAuthHttpSession native_http;

    std::string current_url = auth_url;
    std::string key;
    for (int redirect = 0; redirect < 12; ++redirect)
    {
        if (current_url.rfind("https://login.nvgs.nvidia.com/", 0) == 0)
        {
            key = GetQueryValue(current_url, "key");
            if (!key.empty())
                break;
        }
        const NativeAuthResponse response = native_http.Request("GET", current_url);
        if (response.location.empty())
            break;
        current_url = ResolveRedirectUrl(current_url, response.location);
    }
    if (key.empty())
        throw NativeLoginFallbackRequired("NVIDIA did not offer its native password sign-in flow");
    AppendAuthLog("auth-native: OAuth context initialized");

    JsonPtr account_body(json_object(), &json_decref);
    json_object_set_new(account_body.get(), "email", json_string(Trim(email).c_str()));
    json_object_set_new(account_body.get(), "rememberLogin", json_true());
    json_object_set_new(account_body.get(), "autoLogin", json_false());
    json_object_set_new(account_body.get(), "deviceId", json_string(GenerateDeviceId().c_str()));
    JsonPtr account = NativeJsonRequest(
        native_http, "POST",
        "https://accounts.nvgs.nvidia.com/api/1/frontend/oauth/account/check",
        key, account_body.get(), true);
    key = GetString(account.get(), "key");
    if (key.empty())
        throw std::runtime_error("NVIDIA account check did not return a login context");

    NativeStage stage = AdvanceNativeStage(native_http, key);
    bool password_submitted = false;
    for (int transitions = 0; transitions < 24; ++transitions)
    {
        if (is_cancelled && is_cancelled())
            throw std::runtime_error("NVIDIA sign-in was cancelled");
        const auth::NativeStageAction stage_action = auth::ClassifyNativeStage(stage.page);
        if (stage_action == auth::NativeStageAction::Password)
        {
            if (password_submitted)
                throw std::runtime_error("NVIDIA rejected the supplied account credentials");
            JsonPtr body(json_object(), &json_decref);
            json_object_set_new(body.get(), "password", json_string(password.c_str()));
            json_object_set_new(body.get(), "rememberLogin", json_true());
            json_object_set_new(body.get(), "deviceId", json_string(GenerateDeviceId().c_str()));
            JsonPtr result = NativeJsonRequest(
                native_http, "POST",
                "https://accounts.nvgs.nvidia.com/api/1/frontend/oauth/user/login/password",
                stage.key, body.get(), true);
            key = GetString(result.get(), "key");
            if (key.empty())
                throw std::runtime_error("NVIDIA password login did not return a login context");
            password_submitted = true;
            AppendAuthLog("auth-native: password accepted");
            stage = AdvanceNativeStage(native_http, key);
            continue;
        }

        if (stage_action == auth::NativeStageAction::SelectMfa)
        {
            const std::string capability = UrlEncode("[\"TOTP\"]");
            JsonPtr factors = NativeJsonRequest(
                native_http, "GET",
                "https://accounts.nvgs.nvidia.com/api/1/frontend/oauth/auth/factor/challenge/factors"
                "?browserCapability=" + capability,
                stage.key, nullptr);
            json_t* values = json_object_get(factors.get(), "values");
            if (!json_is_array(values))
            {
                json_t* nested = json_object_get(factors.get(), "factors");
                values = json_is_object(nested) ? json_object_get(nested, "values") : nested;
            }
            json_t* selected_totp = nullptr;
            json_t* selected_email = nullptr;
            std::vector<std::string> factor_types;
            if (json_is_array(values))
            {
                size_t index = 0;
                json_t* item = nullptr;
                json_array_foreach(values, index, item)
                {
                    const std::string type = GetString(item, "type");
                    const std::string normalized = Lowercase(type);
                    if (!type.empty())
                        factor_types.push_back(type);
                    if (normalized == "totp")
                        selected_totp = item;
                    else if (normalized == "email" || normalized == "emailcode")
                        selected_email = item;
                }
            }
            std::string factor_summary;
            for (const std::string& type : factor_types)
            {
                if (!factor_summary.empty())
                    factor_summary += ",";
                factor_summary += type;
            }
            AppendAuthLog(
                "auth-native: MFA factors count=" + std::to_string(factor_types.size()) +
                " types=" + (factor_summary.empty() ? std::string("none") : factor_summary));
            json_t* selected = selected_totp ? selected_totp : selected_email;
            if (!selected)
                throw NativeLoginFallbackRequired(
                    "This account requires a security key, passkey, email approval, or another unsupported verification method");

            std::string factor_id = GetString(selected, "id");
            json_t* descriptors = json_object_get(selected, "descriptors");
            if (factor_id.empty() && json_is_array(descriptors) && json_array_size(descriptors) > 0)
                factor_id = GetString(json_array_get(descriptors, 0), "id");

            const std::string selected_type = GetString(selected, "type");
            JsonPtr init_body(json_object(), &json_decref);
            json_object_set_new(init_body.get(), "type", json_string(selected_type.c_str()));
            if (factor_id.empty())
                json_object_set_new(init_body.get(), "id", json_null());
            else
                json_object_set_new(init_body.get(), "id", json_string(factor_id.c_str()));
            JsonPtr initialized = NativeJsonRequest(
                native_http, "POST",
                "https://accounts.nvgs.nvidia.com/api/1/frontend/oauth/auth/factor/challenge/initialize",
                stage.key, init_body.get());
            key = GetString(initialized.get(), "key");
            if (key.empty())
                throw std::runtime_error("NVIDIA MFA initialization did not return a login context");
            AppendAuthLog("auth-native: MFA initialized type=" + selected_type);
            stage = AdvanceNativeStage(native_http, key);
            continue;
        }

        if (stage_action == auth::NativeStageAction::VerifyTotp)
        {
            if (!request_one_time_code)
                throw NativeLoginFallbackRequired("A NVIDIA authenticator code is required");
            const std::string otp = Trim(request_one_time_code("Enter the 6-digit NVIDIA authenticator code"));
            if (otp.empty())
                throw std::runtime_error("NVIDIA verification was cancelled");
            JsonPtr verify_body(json_object(), &json_decref);
            json_object_set_new(verify_body.get(), "otp", json_string(otp.c_str()));
            JsonPtr verified = NativeJsonRequest(
                native_http, "POST",
                "https://accounts.nvgs.nvidia.com/api/1/frontend/oauth/auth/factor/challenge/verify",
                stage.key, verify_body.get());
            key = GetString(verified.get(), "key");
            if (key.empty())
                throw std::runtime_error("NVIDIA MFA verification did not return a login context");
            AppendAuthLog("auth-native: TOTP accepted");
            stage = AdvanceNativeStage(native_http, key);
            continue;
        }

        if (stage_action == auth::NativeStageAction::WaitForEmail)
        {
            if (report_status)
                report_status("NVIDIA sent an approval email. Open it and approve this sign-in.");
            AppendAuthLog("auth-native: waiting for email approval");
            bool approved = false;
            for (int attempt = 1; attempt <= 60; ++attempt)
            {
                if (is_cancelled && is_cancelled())
                    throw std::runtime_error("NVIDIA sign-in was cancelled");
                if (report_status && (attempt == 1 || attempt % 5 == 0))
                {
                    const int elapsed_seconds = (attempt - 1) * 3;
                    report_status(
                        "NVIDIA sent an approval email. Open it and approve this sign-in. "
                        "Waiting: " + std::to_string(elapsed_seconds) +
                        " seconds (timeout: 180 seconds). Check Spam if needed.");
                }
                const NativeAuthResponse response = native_http.Request(
                    "GET",
                    "https://accounts.nvgs.nvidia.com/api/1/frontend/oauth/user/email/auth/poll",
                    NativeAuthHeaders(stage.key));
                if (response.status_code == 200)
                {
                    JsonPtr result = LoadJson(response.body);
                    key = GetString(result.get(), "key");
                    if (key.empty())
                        throw std::runtime_error("NVIDIA email approval returned no login context");
                    AppendAuthLog("auth-native: email approval accepted attempts=" + std::to_string(attempt));
                    stage = AdvanceNativeStage(native_http, key);
                    approved = true;
                    break;
                }
                if (response.status_code == 202 || response.status_code == 400 ||
                    response.status_code == 404 || response.status_code == 429 ||
                    response.status_code >= 500)
                {
                    if (attempt == 1 || attempt % 10 == 0)
                    {
                        AppendAuthLog(
                            "auth-native: email approval pending HTTP=" +
                            std::to_string(response.status_code) +
                            " attempt=" + std::to_string(attempt));
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    continue;
                }
                throw std::runtime_error(
                    "NVIDIA email approval failed with HTTP " +
                    std::to_string(response.status_code));
            }
            if (!approved)
                throw std::runtime_error("NVIDIA email approval timed out after 3 minutes");
            continue;
        }

        if (stage_action == auth::NativeStageAction::Consent)
        {
            JsonPtr consent(json_object(), &json_decref);
            JsonPtr accepted = NativeJsonRequest(
                native_http, "POST",
                "https://accounts.nvgs.nvidia.com/api/1/frontend/oauth/user/consent",
                stage.key, consent.get());
            key = GetString(accepted.get(), "key");
            if (key.empty())
                throw NativeLoginFallbackRequired("NVIDIA requires an interactive consent screen");
            stage = AdvanceNativeStage(native_http, key);
            continue;
        }

        if (stage_action == auth::NativeStageAction::Advance)
        {
            stage = AdvanceNativeStage(native_http, stage.key);
            continue;
        }

        if (stage_action == auth::NativeStageAction::Finish)
        {
            current_url = stage.external_url.empty()
                ? "https://accounts.nvgs.nvidia.com/api/1/frontend/oauth/user/next?key=" +
                    UrlEncode(stage.key) + "&state=finish"
                : stage.external_url;
            std::string final_url;
            for (int redirect = 0; redirect < 16; ++redirect)
            {
                if (current_url.rfind(kRedirectUri, 0) == 0)
                {
                    final_url = current_url;
                    break;
                }
                const NativeAuthResponse response = native_http.Request("GET", current_url);
                if (response.location.empty())
                    break;
                current_url = ResolveRedirectUrl(current_url, response.location);
            }
            if (final_url.empty())
                throw std::runtime_error("NVIDIA login completed without an OAuth callback");
            if (GetQueryValue(final_url, "state") != state)
                throw std::runtime_error("NVIDIA native login failed the OAuth state check");
            const std::string error = GetQueryValue(final_url, "error");
            if (!error.empty())
                throw std::runtime_error("NVIDIA native login failed: " + error);
            const std::string code = GetQueryValue(final_url, "code");
            if (code.empty())
                throw std::runtime_error("NVIDIA native login did not return an authorization code");

            AuthTokens tokens = ExchangeAuthorizationCode(http_client_, code, pkce.verifier);
            RequestClientToken(http_client_, tokens);
            AuthSession auth_session;
            auth_session.provider = provider;
            auth_session.tokens = std::move(tokens);
            auth_session.user = FetchUserInfo(http_client_, auth_session.tokens);
            auth_session.last_refresh_at_ms = NowMs();
            RefreshMembershipTier(http_client_, auth_session);
            AppendAuthLog("auth-native: login complete");
            return auth_session;
        }

        if (stage.page == "Captcha")
            throw NativeLoginFallbackRequired("NVIDIA requires a CAPTCHA for this sign-in");

        throw NativeLoginFallbackRequired(
            "NVIDIA requires an interactive step not supported by native login: " +
            (stage.page.empty() ? std::string("unknown") : stage.page));
    }

    throw std::runtime_error("NVIDIA native login exceeded the stage transition limit");
}

class QrCallbackServer
{
  public:
    explicit QrCallbackServer(const std::string& auth_url) : auth_url_(auth_url) {}

    ~QrCallbackServer()
    {
        Stop();
    }

    void Start()
    {
        const int server = socket(AF_INET, SOCK_STREAM, 0);
        if (server < 0)
            throw std::runtime_error("QR callback server socket() failed");

#ifndef _WIN32
        int reuse = 1;
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));
#endif

        sockaddr_in address {};
        address.sin_family      = AF_INET;
        address.sin_port        = htons(kLoginCallbackPort);
        address.sin_addr.s_addr = INADDR_ANY;

        if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
        {
            close(server);
            throw std::runtime_error(std::string("QR callback server bind() failed on port ") + std::to_string(kLoginCallbackPort));
        }

        port_ = kLoginCallbackPort;

        if (listen(server, 1) < 0)
        {
            close(server);
            throw std::runtime_error("QR callback server listen() failed");
        }

        AppendAuthLog("auth: QR callback server listening on fixed port " + std::to_string(port_));

        worker_ = std::thread([this, server]() {
            Run(server);
        });
    }

    void Stop()
    {
        stop_ = true;
        if (worker_.joinable())
            worker_.join();
    }

    std::string WaitForResult(std::function<bool()> isCancelled)
    {
        const auto deadline = std::chrono::steady_clock::now() + kQrLoginTimeout;
        while (!completed_ && !stop_)
        {
            if (isCancelled && isCancelled())
            {
                Stop();
                return "";
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                if (completed_)
                    break;
                Stop();
                throw std::runtime_error("GeForce NOW QR login timed out");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!error_.empty())
            throw std::runtime_error(error_);

        return callback_url_;
    }

    int port() const
    {
        return port_;
    }

  private:
    void Complete(std::string callback_url, std::string error = {})
    {
        callback_url_ = std::move(callback_url);
        error_        = std::move(error);
        completed_    = true;
    }

    void SendResponse(int client, bool ok)
    {
        const std::string body =
            ok
                ? "<!doctype html><html><body style=\"font-family:sans-serif;background:#0e1015;color:#fff;padding:32px;text-align:center;\"><h2>SwitchNOW login complete</h2><p style=\"color:#76b900\">You can close this tab and return to your Switch!</p></body></html>"
                : "<!doctype html><html><body style=\"font-family:sans-serif;background:#0e1015;color:#fff;padding:32px;text-align:center;\"><h2 style=\"color:red\">SwitchNOW login failed</h2><p>Please try again.</p></body></html>";

        const std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Connection: close\r\n"
            "Content-Length: " +
            std::to_string(body.size()) + "\r\n\r\n" + body;

        send(client, response.data(), response.size(), 0);
    }

    void Run(int server)
    {
        while (!stop_)
        {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(server, &read_fds);

            timeval timeout {};
            timeout.tv_sec = 1;

            const int ready = select(server + 1, &read_fds, nullptr, nullptr, &timeout);
            if (ready <= 0)
                continue;

            const int client = accept(server, nullptr, nullptr);
            if (client < 0)
                continue;

            char buffer[2048] {};
            const ssize_t read_bytes = recv(client, buffer, sizeof(buffer) - 1, 0);
            if (read_bytes <= 0)
            {
                close(client);
                continue;
            }

            const std::string request(buffer, static_cast<size_t>(read_bytes));
            const auto first_space  = request.find(' ');
            const auto second_space = first_space == std::string::npos
                                          ? std::string::npos
                                          : request.find(' ', first_space + 1);

            std::string target =
                first_space == std::string::npos || second_space == std::string::npos
                    ? std::string("/")
                    : request.substr(first_space + 1, second_space - first_space - 1);

            if (target == "/")
            {
                std::string body = R"html(<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body { font-family: sans-serif; background: #0e1015; color: #fff; padding: 20px; text-align: center; }
.btn { display: inline-block; background: #76b900; color: #000; padding: 15px 30px; text-decoration: none; font-weight: bold; border-radius: 5px; margin: 20px 0; font-size: 18px; }
input { width: 100%; padding: 15px; box-sizing: border-box; margin: 10px 0; border: none; border-radius: 5px; font-size: 16px; color: #000; }
.instructions { text-align: left; background: #1a1e24; padding: 15px; border-radius: 5px; margin-bottom: 20px; font-size: 15px; line-height: 1.4; }
</style></head><body><h2>GeForce NOW Login</h2>
<div class="instructions"><p>This Switch login page always runs on port <b>2259</b>.</p><p>1. Tap the button below to log in to NVIDIA.</p><p>2. After logging in, your browser will show an error like "Site cannot be reached" for <b>localhost:2259</b>.</p>
<p>3. <b>Copy the full URL</b> from your browser's address bar.</p><p>4. Go back to this page and paste the URL below.</p></div>
<a href=")html" + auth_url_ + R"html(" target="_blank" class="btn">Login to NVIDIA</a>
<input type="text" id="pastedUrl" placeholder="Paste the localhost URL here...">
<a href="#" class="btn" onclick="submitUrl()">Submit</a>
<script>
function submitUrl() {
  var url = document.getElementById('pastedUrl').value;
  if (!url) return;
  window.location.href = '/callback?url=' + encodeURIComponent(url);
}
</script></body></html>)html";

                const std::string response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html; charset=utf-8\r\n"
                    "Connection: close\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

                send(client, response.data(), response.size(), 0);
                close(client);
                continue;
            }

            if (target.find("/callback?url=") == 0)
            {
                std::string pasted_url = UrlDecode(target.substr(14));
                target = pasted_url;
            }

            const bool has_code  = !GetQueryValue(target, "code").empty();
            const bool has_error = !GetQueryValue(target, "error").empty();
            const bool is_callback = has_code || has_error;

            if (is_callback) {
                SendResponse(client, has_code && !has_error);
            } else {
                const std::string response = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                send(client, response.data(), response.size(), 0);
            }

            close(client);

            if (is_callback)
            {
                Complete(target);
                break;
            }
        }

        close(server);
    }

    std::atomic<bool> stop_ {false};
    std::thread worker_;
    std::atomic<bool> completed_ {false};
    std::string callback_url_;
    std::string error_;
    std::string auth_url_;
    int port_ = 0;
};

AuthSession GfnClient::ReauthenticateWithSavedCredentials(
    const AuthSession& expired_session,
    const std::function<void(const std::string&)>& report_status,
    const std::function<bool()>& is_cancelled) const
{
    // Store, Library and the periodic account check can notice the same
    // expired token together. Keep native login single-flight and reuse a
    // session another request has just restored.
    std::lock_guard<std::mutex> reauthentication_lock(g_reauthentication_mutex);
    for (const AuthSession& candidate : LoadSavedSessions())
    {
        const bool same_user = candidate.user.user_id == expired_session.user.user_id;
        const bool newer_login = candidate.last_refresh_at_ms > expired_session.last_refresh_at_ms;
        const bool different_token =
            candidate.tokens.access_token != expired_session.tokens.access_token;
        if (!same_user || !newer_login || !different_token)
            continue;

        try
        {
            AuthSession restored = EnsureFreshSavedSession(candidate);
            restored.reauthentication_required = false;
            AppendAuthLog("auth: reused quick sign-in completed by another request");
            return restored;
        }
        catch (const ReauthenticationRequired&)
        {
            // The newer token was rejected too; continue with the remembered
            // credentials while holding the single-flight lock.
        }
    }

    const std::optional<NativeCredentials> stored =
        LoadNativeCredentials(expired_session.provider.idp_id);
    if (!stored || stored->email.empty() || stored->password.empty())
    {
        throw ReauthenticationRequired(
            "The NVIDIA session expired and no password is saved for automatic sign-in.");
    }

    NativeCredentials credentials = *stored;
    AppendAuthLog("auth: automatic quick sign-in started provider=" +
                  expired_session.provider.code);
    try
    {
        AuthSession restored = LoginNative(
            expired_session.provider,
            credentials.email,
            credentials.password,
            [](const std::string&) { return std::string {}; },
            report_status,
            is_cancelled);
        std::fill(credentials.password.begin(), credentials.password.end(), '\0');
        restored.persistence_enabled = true;
        restored.reauthentication_required = false;
        SaveSession(restored);
        AppendAuthLog("auth: automatic quick sign-in completed user_id_present=" +
                      std::string(restored.user.user_id.empty() ? "no" : "yes"));
        return restored;
    }
    catch (...)
    {
        std::fill(credentials.password.begin(), credentials.password.end(), '\0');
        AppendAuthLog("auth: automatic quick sign-in failed");
        throw;
    }
}

AuthSession GfnClient::RecoverSavedSession(
    const AuthSession& session,
    bool force_refresh,
    const std::function<void(const std::string&)>& report_status,
    const std::function<bool()>& is_cancelled) const
{
    if (session.reauthentication_required)
    {
        if (report_status)
            report_status("Restoring the remembered NVIDIA account.");
        return ReauthenticateWithSavedCredentials(session, report_status, is_cancelled);
    }

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
        if (report_status)
            report_status("Saved authorization expired. Starting quick sign-in.");
        return ReauthenticateWithSavedCredentials(session, report_status, is_cancelled);
    }
}

AuthSession GfnClient::LoginSwitchQR(const LoginProvider& provider, std::function<void(const std::string&)> onUrlGenerated, std::function<bool()> isCancelled) const
{
    AppendAuthLog("auth: QR login started provider=" + provider.code);

    const PkcePair pkce = GeneratePkce();
    const std::string state = HexEncode(GenerateRandomBytes(32).data(), 32);

    std::string local_ip = NetworkUtils::GetLocalIPAddress();
    if (local_ip == "127.0.0.1" || local_ip.empty()) {
        throw std::runtime_error("Could not determine local IP address. Are you connected to Wi-Fi?");
    }

    const std::string auth_url = BuildAuthorizeUrl(provider, pkce, state, std::string(kRedirectUri));

    QrCallbackServer callback_server(auth_url);
    callback_server.Start();

    std::string switch_url = "http://" + local_ip + ":" + std::to_string(callback_server.port()) + "/";

    if (onUrlGenerated)
        onUrlGenerated(switch_url);

    const std::string final_url = callback_server.WaitForResult(isCancelled);
    if (final_url.empty())
    {
        throw std::runtime_error("Login was cancelled or timed out.");
    }

    if (GetQueryValue(final_url, "state") != state)
        throw std::runtime_error("GeForce NOW login callback failed the OAuth state check");

    const std::string error = GetQueryValue(final_url, "error");
    if (!error.empty())
    {
        throw std::runtime_error("GeForce NOW login failed: " + error);
    }

    const std::string code = GetQueryValue(final_url, "code");
    if (code.empty())
        throw std::runtime_error("The GeForce NOW login callback did not include an authorization code");

    AuthTokens tokens = ExchangeAuthorizationCode(http_client_, code, pkce.verifier);
    AppendAuthLog("auth: token exchange ok");
    RequestClientToken(http_client_, tokens);

    AuthSession session;
    session.provider = provider;
    session.tokens   = tokens;
    session.user     = FetchUserInfo(http_client_, tokens);
    session.last_refresh_at_ms = NowMs();
    RefreshMembershipTier(http_client_, session);
    AppendAuthLog("auth: user info ok user_id_present=" + std::string(session.user.user_id.empty() ? "no" : "yes"));
    return session;
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
