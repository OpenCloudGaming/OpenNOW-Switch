#include "gfn_client.hpp"
#include "play_history.hpp"

#ifdef __SWITCH__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <switch.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <jansson.h>
#include <curl/curl.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>

#include "network_utils.hpp"
#include "auth_policy.hpp"
#include "native_auth_policy.hpp"
#include "stream_settings.hpp"
#include "stream_diagnostics.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace opennow
{
std::vector<AuthSession> LoadAccountsFromDisk(std::string* active_user_id);
void SaveAccountsToDisk(const std::vector<AuthSession>& sessions, const std::string& active_user_id);

namespace
{

using JsonPtr = std::unique_ptr<json_t, decltype(&json_decref)>;

constexpr const char* kServiceUrlsEndpoint = "https://pcs.geforcenow.com/v1/serviceUrls";
constexpr const char* kPublicCatalogEndpoint =
    "https://static.nvidiagrid.net/supported-public-game-list/locales/gfnpc-en-US.json";
constexpr const char* kTokenEndpoint       = "https://login.nvidia.com/token";
constexpr const char* kClientTokenEndpoint = "https://login.nvidia.com/client_token";
constexpr const char* kUserInfoEndpoint    = "https://login.nvidia.com/userinfo";
constexpr const char* kAuthorizeEndpoint   = "https://login.nvidia.com/authorize";
constexpr const char* kGraphQlEndpoint     = "https://games.geforce.com/graphql";

constexpr const char* kClientId          = "ZU7sPN-miLujMD95LfOQ453IB0AtjM8sMyvgJ9wCXEQ";
constexpr const char* kClientVersion     = "2.0.80.173";
constexpr const char* kLcarsClientId     = "ec7e38d4-03af-4b58-b131-cfb0495903ab";
constexpr const char* kScopes            = "openid consent email tk_client age";
constexpr const char* kDefaultLocale     = "en_US";
constexpr const char* kDefaultProviderId = "PDiAhv2kJTFeQ7WOPqiQ2tRZ7lGhR2X11dXvM4TZSxg";
constexpr const char* kRedirectUri       = "http://localhost:2259";
constexpr int kLoginCallbackPort         = 2259;

constexpr const char* kPanelsQueryHash          = "f8e26265a5db5c20e1334a6872cf04b6e3970507697f6ae55a6ddefa5420daf0";
constexpr const char* kLibraryWithTimeQueryHash = "039e8c0d553972975485fee56e59f2549d2fdb518e247a42ab5022056a74406f";

constexpr const char* kNvidiaFileOrigin  = "https://nvfile";
constexpr const char* kNvidiaFileReferer = "https://nvfile/";
constexpr const char* kPlayOrigin        = "https://play.geforcenow.com";
constexpr const char* kPlayReferer       = "https://play.geforcenow.com/";

constexpr std::int64_t kRefreshWindowMs = 10LL * 60LL * 1000LL;
std::recursive_mutex g_accounts_mutex;
std::mutex g_reauthentication_mutex;
std::string Trim(const std::string& value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
        ++begin;

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;

    return value.substr(begin, end - begin);
}

std::int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string GetAppHome()
{
    return "sdmc:/switch/SwitchNOW";
}

std::string GetSessionPath()
{
    return GetAppHome() + "/auth_session.json";
}

std::string GetAccountsPath()
{
    return GetAppHome() + "/auth_accounts.json";
}

std::string GetDeviceIdPath()
{
    return GetAppHome() + "/device_id.txt";
}

std::string GetLauncherPreferencesPath()
{
    return GetAppHome() + "/launcher_preferences.json";
}

std::string GetActiveCloudSessionPath()
{
    return GetAppHome() + "/active_cloud_session.json";
}

std::string GetNativeCredentialsPath()
{
    return GetAppHome() + "/auth_credentials.vault";
}

void EnsureAppHome()
{
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
#endif
}

JsonPtr LoadJson(const std::string& body)
{
    json_error_t error {};
    JsonPtr root(json_loads(body.c_str(), 0, &error), &json_decref);
    if (!root)
    {
        throw std::runtime_error(
            "JSON parse failed at line " + std::to_string(error.line) + ": " + error.text);
    }

    return root;
}

std::string JsonString(json_t* value)
{
    if (json_is_string(value))
    {
        const char* raw = json_string_value(value);
        return raw ? raw : "";
    }

    if (json_is_integer(value))
        return std::to_string(json_integer_value(value));

    return "";
}

std::string GetString(json_t* object, const char* key)
{
    if (!object || !json_is_object(object))
        return "";

    return JsonString(json_object_get(object, key));
}

bool GetBool(json_t* object, const char* key, bool fallback = false)
{
    if (!object || !json_is_object(object))
        return fallback;

    json_t* value = json_object_get(object, key);
    if (json_is_boolean(value))
        return json_boolean_value(value);

    return fallback;
}

int GetInteger(json_t* object, const char* key, int fallback = 0)
{
    if (!object || !json_is_object(object))
        return fallback;

    json_t* value = json_object_get(object, key);
    if (!json_is_integer(value))
        return fallback;

    return static_cast<int>(json_integer_value(value));
}

bool HasPrefix(const std::string& value, const char* prefix)
{
    return value.rfind(prefix, 0) == 0;
}

std::string ExtractHostFromUrlLike(const std::string& value)
{
    size_t start = value.find("://");
    if (start == std::string::npos)
        return "";

    start += 3;
    const size_t path = value.find('/', start);
    std::string host_port = value.substr(start, path == std::string::npos ? std::string::npos : path - start);

    if (!host_port.empty() && host_port.front() == '[') {
        const size_t close = host_port.find(']');
        return close == std::string::npos ? "" : host_port.substr(1, close - 1);
    }

    const size_t colon = host_port.find(':');
    return colon == std::string::npos ? host_port : host_port.substr(0, colon);
}

int ExtractPortFromUrlLike(const std::string& value)
{
    size_t start = value.find("://");
    if (start == std::string::npos)
        return 0;

    start += 3;
    const size_t path = value.find('/', start);
    const size_t host_end = path == std::string::npos ? value.size() : path;

    size_t colon = std::string::npos;
    if (start < value.size() && value[start] == '[') {
        const size_t close = value.find(']', start);
        if (close != std::string::npos && close + 1 < host_end && value[close + 1] == ':')
            colon = close + 1;
    } else {
        colon = value.find(':', start);
        if (colon >= host_end)
            colon = std::string::npos;
    }

    if (colon == std::string::npos || colon + 1 >= host_end)
        return 0;

    char* end = nullptr;
    long port = std::strtol(value.c_str() + colon + 1, &end, 10);
    if (port <= 0 || port > 65535)
        return 0;

    return static_cast<int>(port);
}

std::string GetConnectionIp(json_t* connection, const std::string& fallback_ip = "")
{
    if (!connection || !json_is_object(connection))
        return fallback_ip;

    json_t* ip = json_object_get(connection, "ip");
    if (json_is_string(ip))
        return JsonString(ip);

    if (json_is_array(ip) && json_array_size(ip) > 0)
        return JsonString(json_array_get(ip, 0));

    const std::string resource_path = GetString(connection, "resourcePath");
    const std::string host = ExtractHostFromUrlLike(resource_path);
    return host.empty() ? fallback_ip : host;
}

int GetConnectionPort(json_t* connection)
{
    const int direct_port = GetInteger(connection, "port");
    if (direct_port > 0)
        return direct_port;

    return ExtractPortFromUrlLike(GetString(connection, "resourcePath"));
}

std::string BuildSignalingUrlFromConnection(json_t* connection, const std::string& fallback_ip)
{
    const std::string resource_path = GetString(connection, "resourcePath");
    const std::string ip = GetConnectionIp(connection, fallback_ip);
    int port = GetConnectionPort(connection);
    if (port <= 0)
        port = 443;

    if (HasPrefix(resource_path, "wss://"))
        return resource_path;

    if (HasPrefix(resource_path, "https://"))
        return "wss://" + resource_path.substr(strlen("https://"));

    if (HasPrefix(resource_path, "rtsps://") || HasPrefix(resource_path, "rtsp://")) {
        const std::string host = ExtractHostFromUrlLike(resource_path);
        return host.empty() ? "" : "wss://" + host + "/nvst/";
    }

    if (!resource_path.empty() && resource_path.front() == '/' && !ip.empty())
        return "wss://" + ip + ":" + std::to_string(port) + resource_path;

    return ip.empty() ? "" : "wss://" + ip + ":443/nvst/";
}

void AppendIceServer(SessionInfo& info, std::string url, std::string username, std::string credential)
{
    url = Trim(url);
    if (url.empty())
        return;

    const bool is_ice_url =
        HasPrefix(url, "stun:") || HasPrefix(url, "stuns:") ||
        HasPrefix(url, "turn:") || HasPrefix(url, "turns:");
    if (!is_ice_url)
        return;

    const auto duplicate = std::find_if(
        info.ice_servers.begin(),
        info.ice_servers.end(),
        [&url](const IceServerInfo& existing) {
            return existing.url == url;
        });
    if (duplicate != info.ice_servers.end())
        return;

    IceServerInfo ice;
    ice.url        = std::move(url);
    ice.username   = std::move(username);
    ice.credential = std::move(credential);
    info.ice_servers.push_back(std::move(ice));
}

void CollectIceServerEntry(json_t* value, SessionInfo& info, const std::string& username = {}, const std::string& credential = {})
{
    if (!value)
        return;

    if (json_is_string(value))
    {
        AppendIceServer(info, JsonString(value), username, credential);
        return;
    }

    if (json_is_array(value))
    {
        size_t index;
        json_t* entry;
        json_array_foreach(value, index, entry)
            CollectIceServerEntry(entry, info, username, credential);
        return;
    }

    if (!json_is_object(value))
        return;

    const std::string entry_username =
        GetString(value, "username").empty() ? username : GetString(value, "username");
    std::string entry_credential = GetString(value, "credential");
    if (entry_credential.empty())
        entry_credential = GetString(value, "password");
    if (entry_credential.empty())
        entry_credential = credential;

    const char* url_keys[] = {
        "urls", "url", "uri", "server", "serverUrl", "stunUrl", "turnUrl"
    };

    for (const char* key : url_keys)
        CollectIceServerEntry(json_object_get(value, key), info, entry_username, entry_credential);
}

void CollectIceServersFromObject(json_t* object, SessionInfo& info)
{
    if (!object || !json_is_object(object))
        return;

    const char* nested_ice_config_keys[] = {
        "iceServerConfiguration",
        "iceConfiguration",
        "rtcConfiguration",
        "webrtcConfiguration",
    };

    for (const char* key : nested_ice_config_keys)
        CollectIceServersFromObject(json_object_get(object, key), info);

    const char* ice_keys[] = {
        "iceServers",
        "ice_servers",
        "iceServer",
        "ice_server",
        "rtcIceServers",
        "webrtcIceServers",
        "stunServers",
        "turnServers",
    };

    for (const char* key : ice_keys)
        CollectIceServerEntry(json_object_get(object, key), info);
}

void ApplySessionNetworkInfo(json_t* sess, SessionInfo& info)
{
    if (!sess || !json_is_object(sess))
        return;

    CollectIceServersFromObject(sess, info);

    const std::string session_token = GetString(sess, "sessionToken");
    if (!session_token.empty())
        info.session_token = session_token;

    const std::string server_ip = GetString(sess, "serverIp");
    if (!server_ip.empty())
        info.server_ip = server_ip;

    json_t* session_control = json_object_get(sess, "sessionControlInfo");
    CollectIceServersFromObject(session_control, info);
    if (info.server_ip.empty() && session_control)
        info.server_ip = GetString(session_control, "ip");

    const std::string signaling_url = GetString(sess, "signalingUrl");
    if (!signaling_url.empty())
        info.signaling_url = signaling_url;

    json_t* connection_info_array = json_object_get(sess, "connectionInfo");
    if (connection_info_array && json_is_array(connection_info_array)) {
        const int priorities[] = {2, 17, 14};
        for (int priority : priorities) {
            std::string best_ip;
            int best_port = 0;

            size_t index;
            json_t* conn_info;
            json_array_foreach(connection_info_array, index, conn_info) {
                CollectIceServersFromObject(conn_info, info);

                if (GetInteger(conn_info, "usage") != priority)
                    continue;

                const std::string fallback_ip = priority == 14 ? info.server_ip : "";
                const std::string ip = GetConnectionIp(conn_info, fallback_ip);
                const int port = GetConnectionPort(conn_info);
                if (!ip.empty() && port > 0 && (priority != 14 || port > best_port)) {
                    best_ip = ip;
                    best_port = port;
                }
            }

            if (!best_ip.empty() && best_port > 0) {
                info.media_ip = best_ip;
                info.media_port = best_port;
                break;
            }
        }

        if (info.signaling_url.empty()) {
            size_t index;
            json_t* conn_info;
            json_array_foreach(connection_info_array, index, conn_info) {
                CollectIceServersFromObject(conn_info, info);

                if (GetInteger(conn_info, "usage") != 14)
                    continue;

                info.signaling_url = BuildSignalingUrlFromConnection(conn_info, info.server_ip);
                if (!info.signaling_url.empty())
                    break;
            }
        }
    }

    if (info.signaling_url.empty() && session_control)
        info.signaling_url = BuildSignalingUrlFromConnection(session_control, info.server_ip);
}

std::string EnsureTrailingSlash(std::string value)
{
    if (value.empty() || value.back() == '/')
        return value;

    value.push_back('/');
    return value;
}

LoginProvider DefaultProvider()
{
    LoginProvider provider;
    provider.idp_id                = kDefaultProviderId;
    provider.code                  = "NVIDIA";
    provider.display_name          = "NVIDIA";
    provider.streaming_service_url = "https://prod.cloudmatchbeta.nvidiagrid.net/";
    provider.priority              = 0;
    return provider;
}

std::vector<unsigned char> GenerateRandomBytes(size_t length)
{
    std::vector<unsigned char> bytes(length);

#ifdef __SWITCH__
    randomGet(bytes.data(), bytes.size());
#else
    std::random_device device;
    for (auto& byte : bytes)
        byte = static_cast<unsigned char>(device());
#endif

    return bytes;
}

std::string HexEncode(const unsigned char* data, size_t length)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string output;
    output.reserve(length * 2);

    for (size_t index = 0; index < length; ++index)
    {
        output.push_back(kDigits[(data[index] >> 4) & 0x0F]);
        output.push_back(kDigits[data[index] & 0x0F]);
    }

    return output;
}

std::vector<unsigned char> HexDecode(const std::string& input)
{
    if ((input.size() & 1U) != 0)
        throw std::runtime_error("Invalid vault hex length");

    auto value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };

    std::vector<unsigned char> output(input.size() / 2);
    for (size_t i = 0; i < output.size(); ++i)
    {
        const int high = value(input[i * 2]);
        const int low = value(input[i * 2 + 1]);
        if (high < 0 || low < 0)
            throw std::runtime_error("Invalid vault hex data");
        output[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return output;
}

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

std::string ReadTextFile(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
        return "";

    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void WriteTextFile(const std::string& path, const std::string& content)
{
    EnsureAppHome();

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
        throw std::runtime_error("Unable to write " + path);

    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
}

void WriteTextFileAtomically(const std::string& path, const std::string& content)
{
    const std::string temporary = path + ".tmp";
    const std::string backup = path + ".bak";
    WriteTextFile(temporary, content);

    std::remove(backup.c_str());
    const bool had_original = std::rename(path.c_str(), backup.c_str()) == 0;
    if (std::rename(temporary.c_str(), path.c_str()) == 0)
        return;

    if (had_original)
        std::rename(backup.c_str(), path.c_str());
    std::remove(temporary.c_str());
    throw std::runtime_error("Unable to replace " + path);
}

std::string FormatResult(Result rc)
{
    char buffer[32] {};
    std::snprintf(buffer, sizeof(buffer), "0x%08x", static_cast<unsigned int>(rc));
    return std::string(buffer);
}

std::string AuthSessionHealth(const AuthSession& session)
{
    const std::int64_t remaining_ms = session.tokens.expires_at_ms - NowMs();
    const std::int64_t remaining_minutes = remaining_ms > 0 ? remaining_ms / 60000 : 0;
    return "user=" + std::string(session.user.user_id.empty() ? "missing" : "present") +
           " accessMin=" + std::to_string(remaining_minutes) +
           " refresh=" + std::string(session.tokens.refresh_token.empty() ? "missing" : "present") +
           " client=" + std::string(session.tokens.client_token.empty() ? "missing" : "present") +
           " persistent=" + std::to_string(session.persistence_enabled ? 1 : 0);
}

void AppendAuthLog(const std::string& line)
{
    if (!StreamDiagnosticsEnabled())
        return;
    EnsureAppHome();

    std::ofstream stream(GetAppHome() + "/auth.log", std::ios::app);
    if (!stream.is_open())
        return;

    stream << '[' << NowMs() << "] " << line << '\n';
}

std::string GetSessionTracePath()
{
    return GetAppHome() + "/session_trace.log";
}

std::string SessionTraceHint()
{
    return StreamDiagnosticsEnabled()
        ? "\nDetails: sdmc:/switch/SwitchNOW/session_trace.log"
        : "\nEnable Settings > Stream > Debug diagnostics for a detailed trace.";
}

void ResetSessionTraceLog(const std::string& reason)
{
    if (!StreamDiagnosticsEnabled())
        return;
    EnsureAppHome();

    std::ofstream stream(GetSessionTracePath(), std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
        return;

    stream << "SwitchNOW session trace\n";
    stream << "reason=" << reason << "\n";
    stream << "timestamp_ms=" << NowMs() << "\n\n";
}

void AppendSessionTraceLog(const std::string& line)
{
    if (!StreamDiagnosticsEnabled())
        return;
    EnsureAppHome();

    std::ofstream stream(GetSessionTracePath(), std::ios::binary | std::ios::app);
    if (!stream.is_open())
        return;

    stream << "[" << NowMs() << "] " << line << '\n';
}

std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool IsSensitiveJsonKey(const std::string& key)
{
    const std::string lower = Lowercase(key);
    return lower.find("token") != std::string::npos ||
           lower.find("authorization") != std::string::npos ||
           lower.find("credential") != std::string::npos ||
           lower.find("password") != std::string::npos ||
           lower.find("secret") != std::string::npos ||
           lower.find("jwt") != std::string::npos;
}

void RedactJsonInPlace(json_t* value)
{
    if (!value)
        return;

    if (json_is_object(value))
    {
        void* iter = json_object_iter(value);
        while (iter)
        {
            const char* key = json_object_iter_key(iter);
            json_t* child = json_object_iter_value(iter);
            void* next = json_object_iter_next(value, iter);

            if (key && IsSensitiveJsonKey(key))
                json_object_set_new(value, key, json_string("<redacted>"));
            else
                RedactJsonInPlace(child);

            iter = next;
        }
        return;
    }

    if (json_is_array(value))
    {
        size_t index;
        json_t* child;
        json_array_foreach(value, index, child)
            RedactJsonInPlace(child);
    }
}

std::string JsonForTrace(const std::string& body)
{
    if (body.empty())
        return "<empty>";

    json_error_t error {};
    JsonPtr root(json_loads(body.c_str(), 0, &error), &json_decref);
    if (!root)
        return body;

    RedactJsonInPlace(root.get());
    char* dump = json_dumps(root.get(), JSON_INDENT(2));
    if (!dump)
        return body;

    std::unique_ptr<char, decltype(&std::free)> output(dump, &std::free);
    return output.get();
}

std::string HeadersForTrace(const std::vector<std::string>& headers)
{
    std::ostringstream out;
    for (const std::string& header : headers)
    {
        const std::string lower = Lowercase(header);
        if (lower.rfind("authorization:", 0) == 0)
            out << "Authorization: <redacted>\n";
        else
            out << header << "\n";
    }
    return out.str();
}

std::string IntFieldText(json_t* object, const char* key)
{
    if (!object || !json_is_object(object))
        return "";

    json_t* value = json_object_get(object, key);
    if (json_is_integer(value))
        return std::to_string(json_integer_value(value));

    if (json_is_string(value))
        return JsonString(value);

    return "";
}

std::string CloudMatchErrorDetails(int http_status, json_t* root)
{
    std::ostringstream out;
    out << "HTTP " << http_status;

    json_t* req_status = root ? json_object_get(root, "requestStatus") : nullptr;
    if (req_status)
    {
        const std::string status_code = IntFieldText(req_status, "statusCode");
        const std::string status_description = GetString(req_status, "statusDescription");
        const std::string unified_error = IntFieldText(req_status, "unifiedErrorCode");

        if (!status_code.empty())
            out << ", statusCode=" << status_code;
        if (!status_description.empty())
            out << ", statusDescription=" << status_description;
        if (!unified_error.empty())
            out << ", unifiedErrorCode=" << unified_error;
    }

    json_t* sess = root ? json_object_get(root, "session") : nullptr;
    if (sess)
    {
        const std::string session_id = GetString(sess, "sessionId");
        const std::string session_status = IntFieldText(sess, "status");
        const std::string error_code = IntFieldText(sess, "errorCode");
        const std::string error_description = GetString(sess, "errorDescription");

        if (!session_id.empty())
            out << ", sessionId=" << session_id;
        if (!session_status.empty())
            out << ", sessionStatus=" << session_status;
        if (!error_code.empty())
            out << ", sessionErrorCode=" << error_code;
        if (!error_description.empty())
            out << ", sessionErrorDescription=" << error_description;
    }

    return out.str();
}

std::string BuildCloudMatchErrorMessage(const std::string& stage, int http_status, json_t* root)
{
    const std::string details = CloudMatchErrorDetails(http_status, root);
    std::string message = stage + " failed: " + details;
    json_t* request_status = root ? json_object_get(root, "requestStatus") : nullptr;
    if (request_status && GetInteger(request_status, "statusCode") == 81) {
        message += "\nThis game is not available with the current GeForce NOW membership. "
                   "If the game page says Premium, upgrade the membership to play it.";
    }
    return message + SessionTraceHint();
}

bool IsAppPatchingResponse(json_t* root)
{
    json_t* request_status = root ? json_object_get(root, "requestStatus") : nullptr;
    if (!request_status || GetInteger(request_status, "statusCode") != 41)
        return false;

    return GetString(request_status, "statusDescription").find("APP_PATCHING_STATUS") != std::string::npos;
}

void ApplySeatSetupInfo(json_t* sess, SessionInfo& info)
{
    json_t* seat_setup = sess ? json_object_get(sess, "seatSetupInfo") : nullptr;
    if (!seat_setup || !json_is_object(seat_setup))
        return;

    json_t* queue_position = json_object_get(seat_setup, "queuePosition");
    if (queue_position && json_is_integer(queue_position))
        info.queue_position = json_integer_value(queue_position);
}

std::string GenerateDeviceId()
{
    std::string stored = Trim(ReadTextFile(GetDeviceIdPath()));
    if (!stored.empty())
        return stored;

    const auto bytes = GenerateRandomBytes(32);
    stored           = HexEncode(bytes.data(), bytes.size());
    WriteTextFileAtomically(GetDeviceIdPath(), stored);
    return stored;
}

constexpr const char* kTokenVaultHeader = "OPENNOW_TOKEN_VAULT_V1";
constexpr const char* kTokenVaultAad = "OpenNOW Switch token vault v1";

std::array<unsigned char, 32> TokenVaultKey()
{
    const std::string material =
        "OpenNOW-Switch-vault-key-v1|" + GenerateDeviceId() + "|05004F4E4F575358";
    std::array<unsigned char, 32> key {};
    if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(material.data()),
                       material.size(), key.data(), 0) != 0)
    {
        throw std::runtime_error("Unable to derive token vault key");
    }
    return key;
}

std::string EncryptTokenVault(const std::string& plaintext)
{
    const auto key = TokenVaultKey();
    const auto nonce = GenerateRandomBytes(12);
    std::array<unsigned char, 16> tag {};
    std::vector<unsigned char> ciphertext(plaintext.size());

    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int rc = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key.data(), 256);
    if (rc == 0)
    {
        rc = mbedtls_gcm_crypt_and_tag(
            &context, MBEDTLS_GCM_ENCRYPT, plaintext.size(), nonce.data(), nonce.size(),
            reinterpret_cast<const unsigned char*>(kTokenVaultAad), std::strlen(kTokenVaultAad),
            reinterpret_cast<const unsigned char*>(plaintext.data()), ciphertext.data(),
            tag.size(), tag.data());
    }
    mbedtls_gcm_free(&context);
    if (rc != 0)
        throw std::runtime_error("Unable to encrypt token vault");

    return std::string(kTokenVaultHeader) + "\n" +
           HexEncode(nonce.data(), nonce.size()) + "\n" +
           HexEncode(tag.data(), tag.size()) + "\n" +
           HexEncode(ciphertext.data(), ciphertext.size()) + "\n";
}

std::string DecryptTokenVault(const std::string& encoded)
{
    std::istringstream input(encoded);
    std::string header;
    std::string nonce_hex;
    std::string tag_hex;
    std::string ciphertext_hex;
    std::getline(input, header);
    std::getline(input, nonce_hex);
    std::getline(input, tag_hex);
    std::getline(input, ciphertext_hex);
    if (header != kTokenVaultHeader)
        return encoded;

    const auto nonce = HexDecode(nonce_hex);
    const auto tag = HexDecode(tag_hex);
    const auto ciphertext = HexDecode(ciphertext_hex);
    if (nonce.size() != 12 || tag.size() != 16 || ciphertext.empty())
        throw std::runtime_error("Invalid token vault structure");

    const auto key = TokenVaultKey();
    std::string plaintext(ciphertext.size(), '\0');
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int rc = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key.data(), 256);
    if (rc == 0)
    {
        rc = mbedtls_gcm_auth_decrypt(
            &context, ciphertext.size(), nonce.data(), nonce.size(),
            reinterpret_cast<const unsigned char*>(kTokenVaultAad), std::strlen(kTokenVaultAad),
            tag.data(), tag.size(), ciphertext.data(),
            reinterpret_cast<unsigned char*>(plaintext.data()));
    }
    mbedtls_gcm_free(&context);
    if (rc != 0)
        throw std::runtime_error("Token vault authentication failed");
    return plaintext;
}

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

std::string UrlEncode(const std::string& input, bool plus_for_space = false)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";

    std::string output;
    output.reserve(input.size() * 3);

    for (unsigned char ch : input)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~')
        {
            output.push_back(static_cast<char>(ch));
        }
        else if (plus_for_space && ch == ' ')
        {
            output.push_back('+');
        }
        else
        {
            output.push_back('%');
            output.push_back(kDigits[(ch >> 4) & 0x0F]);
            output.push_back(kDigits[ch & 0x0F]);
        }
    }

    return output;
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

std::vector<std::string> BuildGfnLcarsHeaders(
    const std::string& token,
    const std::string& client_type,
    const std::string& client_streamer,
    bool include_user_agent)
{
    std::vector<std::string> headers;
    headers.push_back("Accept: application/json");
    headers.push_back("nv-client-id: " + std::string(kLcarsClientId));
    headers.push_back("nv-client-type: " + client_type);
    headers.push_back("nv-client-version: " + std::string(kClientVersion));
    headers.push_back("nv-client-streamer: " + client_streamer);
    headers.push_back("nv-device-os: WINDOWS");
    headers.push_back("nv-device-type: DESKTOP");

    if (!token.empty())
        headers.push_back("Authorization: GFNJWT " + token);

    if (include_user_agent)
        headers.push_back("User-Agent: " + std::string(GfnClient::kUserAgent));

    return headers;
}

std::vector<std::string> BuildGraphQlHeaders(const std::string& token)
{
    std::vector<std::string> headers;
    headers.push_back("Accept: application/json, text/plain, */*");
    // NVIDIA's Switch-facing persisted-query endpoint rejects this GET as
    // malformed JSON. application/graphql keeps both library and play-time
    // fields available while preserving the endpoint's expected request type.
    headers.push_back("Content-Type: application/graphql");
    headers.push_back("Origin: " + std::string(kPlayOrigin));
    headers.push_back("Referer: " + std::string(kPlayReferer));
    headers.push_back("nv-client-id: " + std::string(kLcarsClientId));
    headers.push_back("nv-client-type: NATIVE");
    headers.push_back("nv-client-version: " + std::string(kClientVersion));
    headers.push_back("nv-client-streamer: NVIDIA-CLASSIC");
    headers.push_back("nv-device-os: WINDOWS");
    headers.push_back("nv-device-type: DESKTOP");
    headers.push_back("nv-device-make: UNKNOWN");
    headers.push_back("nv-device-model: UNKNOWN");
    headers.push_back("nv-browser-type: CHROME");
    headers.push_back("User-Agent: " + std::string(GfnClient::kUserAgent));

    if (!token.empty())
        headers.push_back("Authorization: GFNJWT " + token);

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

std::string DumpJson(json_t* value)
{
    char* raw = json_dumps(value, JSON_COMPACT);
    if (!raw)
        throw std::runtime_error("Unable to encode the NVIDIA login request");
    std::string result(raw);
    free(raw);
    return result;
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
        throw std::runtime_error("Saved GeForce NOW session cannot be refreshed");

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

    if (tokens.client_token.empty())
    {
        tokens.client_token              = session.tokens.client_token;
        tokens.client_token_expires_at_ms = session.tokens.client_token_expires_at_ms;
    }

    return tokens;
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
    user.membership_tier = "FREE";

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

std::string ResolveSessionJwt(const AuthSession& session)
{
    // CloudMatch expects NVIDIA's signed ID token. OAuth access tokens may be opaque and
    // are rejected as GFNJWT before the session request is parsed.
    return session.tokens.id_token.empty() ? session.tokens.access_token : session.tokens.id_token;
}

std::string OptimizeImageUrl(const std::string& url)
{
    if (url.find("img.nvidiagrid.net") != std::string::npos)
        return url + ";w=272";

    return url;
}

bool IsOwnedLibraryStatus(const std::string& status)
{
    return status == "MANUAL" || status == "PLATFORM_SYNC" || status == "IN_LIBRARY";
}

bool IsNumericId(const std::string& value)
{
    if (value.empty())
        return false;

    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

GameInfo ParseApp(json_t* app)
{
    GameInfo game;
    game.id                 = GetString(app, "id");
    game.uuid               = game.id;
    game.title              = GetString(app, "title");
    game.description        = GetString(app, "description");
    if (game.description.empty())
        game.description = GetString(app, "longDescription");
    if (game.description.empty())
        game.description = GetString(app, "shortDescription");
    game.publisher          = GetString(app, "publisherName");
    json_t* app_gfn = json_object_get(app, "gfn");
    game.membership_tier_label = GetString(app_gfn, "minimumMembershipTierLabel");
    json_t* app_library = app_gfn ? json_object_get(app_gfn, "library") : nullptr;
    game.last_played = GetString(app_library, "lastPlayedDate");
    if (game.last_played.empty())
        game.last_played = GetString(app_library, "lastPlayedAt");

    json_t* images = json_object_get(app, "images");
    for (const char* key : {"KEY_ART", "GAME_BOX_ART", "TV_BANNER", "HERO_IMAGE"})
    {
        const std::string candidate = GetString(images, key);
        if (!candidate.empty())
        {
            game.image_url = OptimizeImageUrl(candidate);
            break;
        }
    }

    json_t* variants = json_object_get(app, "variants");
    if (json_is_array(variants))
    {
        size_t index = 0;
        json_t* entry = nullptr;
        json_array_foreach(variants, index, entry)
        {
            GameVariant variant;
            variant.id    = GetString(entry, "id");
            variant.store = GetString(entry, "appStore");

            json_t* gfn = json_object_get(entry, "gfn");
            variant.gfn_status = GetString(gfn, "status");

            json_t* library = gfn ? json_object_get(gfn, "library") : nullptr;
            variant.library_status   = GetString(library, "status");
            variant.library_selected = GetBool(library, "selected");
            variant.last_played_date = GetString(library, "lastPlayedDate");
            if (variant.last_played_date.empty())
                variant.last_played_date = GetString(library, "lastPlayedAt");
            if (variant.last_played_date.empty())
                variant.last_played_date = GetString(library, "lastPlayed");

            if (variant.library_selected) {
                game.selected_variant_index = index;
                game.launch_app_id = variant.id;
            }

            if (!variant.last_played_date.empty() &&
                (game.last_played.empty() || variant.last_played_date > game.last_played))
                game.last_played = variant.last_played_date;

            if (!variant.store.empty() &&
                std::find(game.available_stores.begin(), game.available_stores.end(), variant.store) ==
                    game.available_stores.end())
            {
                game.available_stores.push_back(variant.store);
            }

            if (IsOwnedLibraryStatus(variant.library_status))
                game.is_in_library = true;

            if (game.launch_app_id.empty() && IsNumericId(variant.id))
                game.launch_app_id = variant.id;

            game.variants.push_back(std::move(variant));
        }
    }

    if (game.launch_app_id.empty() && IsNumericId(game.id))
        game.launch_app_id = game.id;

    return game;
}

void MergeGame(GameInfo& target, const GameInfo& incoming)
{
    if (target.image_url.empty())
        target.image_url = incoming.image_url;

    if (target.description.empty())
        target.description = incoming.description;

    if (target.publisher.empty())
        target.publisher = incoming.publisher;

    if (!incoming.last_played.empty() &&
        (target.last_played.empty() || incoming.last_played > target.last_played))
        target.last_played = incoming.last_played;

    if (target.launch_app_id.empty())
        target.launch_app_id = incoming.launch_app_id;

    if (target.membership_tier_label.empty())
        target.membership_tier_label = incoming.membership_tier_label;

    target.is_in_library = target.is_in_library || incoming.is_in_library;

    for (const auto& store : incoming.available_stores)
    {
        if (std::find(target.available_stores.begin(), target.available_stores.end(), store) ==
            target.available_stores.end())
        {
            target.available_stores.push_back(store);
        }
    }

    for (const auto& variant : incoming.variants)
    {
        const auto it = std::find_if(
            target.variants.begin(),
            target.variants.end(),
            [&variant](const GameVariant& existing) { return existing.id == variant.id; });

        if (it == target.variants.end())
            target.variants.push_back(variant);
    }
}

void ThrowIfGraphQlFailed(json_t* root)
{
    json_t* errors = root ? json_object_get(root, "errors") : nullptr;
    if (!json_is_array(errors) || json_array_size(errors) == 0)
        return;

    std::string message = "GFN GraphQL returned errors";
    json_t* first       = json_array_get(errors, 0);
    const std::string first_message = GetString(first, "message");
    if (!first_message.empty())
        message += ": " + first_message;

    throw std::runtime_error(message);
}

std::string BuildLibraryUrl(const std::string& vpc_id, bool with_library_time)
{
    const std::string variables =
        std::string("{\"vpcId\":\"") + vpc_id + "\",\"locale\":\"" + kDefaultLocale +
        "\",\"panelNames\":[\"LIBRARY\"]}";

    const std::string extensions =
        std::string("{\"persistedQuery\":{\"sha256Hash\":\"") +
        (with_library_time ? kLibraryWithTimeQueryHash : kPanelsQueryHash) + "\"}}";

    std::string url = std::string(kGraphQlEndpoint) + "?";
    url += "requestType=" + UrlEncode("panels/Library");
    url += "&extensions=" + UrlEncode(extensions);
    url += "&huId=" + UrlEncode(HexEncode(GenerateRandomBytes(8).data(), 8));
    url += "&variables=" + UrlEncode(variables);
    return url;
}

std::string ResolveVpcId(const HttpClient& http_client, const AuthSession& session)
{
    const HttpResponse response = http_client.Get(
        EnsureTrailingSlash(session.provider.streaming_service_url) + "v2/serverInfo",
        GfnClient::kUserAgent,
        BuildGfnLcarsHeaders(ResolveSessionJwt(session), "NATIVE", "NVIDIA-CLASSIC", true));

    if (response.status_code != 200)
        return "GFN-PC";

    JsonPtr root = LoadJson(response.body);
    json_t* request_status = json_object_get(root.get(), "requestStatus");
    const std::string server_id = GetString(request_status, "serverId");
    return server_id.empty() ? std::string("GFN-PC") : server_id;
}

std::vector<GameInfo> ParseLibraryGames(JsonPtr& root)
{
    ThrowIfGraphQlFailed(root.get());

    std::unordered_map<std::string, size_t> index_by_id;
    std::vector<GameInfo> games;

    json_t* data   = json_object_get(root.get(), "data");
    json_t* panels = data ? json_object_get(data, "panels") : nullptr;
    if (!json_is_array(panels))
        return {};

    size_t panel_index = 0;
    json_t* panel      = nullptr;
    json_array_foreach(panels, panel_index, panel)
    {
        json_t* sections = json_object_get(panel, "sections");
        if (!json_is_array(sections))
            continue;

        size_t section_index = 0;
        json_t* section      = nullptr;
        json_array_foreach(sections, section_index, section)
        {
            json_t* items = json_object_get(section, "items");
            if (!json_is_array(items))
                continue;

            size_t item_index = 0;
            json_t* item      = nullptr;
            json_array_foreach(items, item_index, item)
            {
                if (GetString(item, "__typename") != "GameItem")
                    continue;

                json_t* app = json_object_get(item, "app");
                if (!json_is_object(app))
                    continue;

                GameInfo game = ParseApp(app);
                if (game.id.empty() || game.title.empty())
                    continue;

                const auto existing = index_by_id.find(game.id);
                if (existing == index_by_id.end())
                {
                    index_by_id.emplace(game.id, games.size());
                    games.push_back(std::move(game));
                }
                else
                {
                    MergeGame(games[existing->second], game);
                }
            }
        }
    }

    return games;
}

PublicGame ToPublicGame(const GameInfo& source)
{
    PublicGame game;
    game.id                    = source.id;
    game.uuid                  = source.uuid;
    game.launch_app_id         = source.launch_app_id;
    game.title                 = source.title;
    game.publisher             = source.publisher;
    game.image_url             = source.image_url;
    game.membership_tier_label = source.membership_tier_label;
    game.is_in_library         = source.is_in_library;
    game.variants              = source.variants;

    if (source.selected_variant_index < source.variants.size())
        game.store = source.variants[source.selected_variant_index].store;
    if (game.store.empty() && !source.available_stores.empty())
        game.store = source.available_stores.front();
    if (game.store.empty())
        game.store = "Unknown";
    return game;
}

std::vector<PublicGame> ParseCatalogPage(
    JsonPtr& root, bool& has_next_page, std::string& end_cursor)
{
    ThrowIfGraphQlFailed(root.get());
    has_next_page = false;
    end_cursor.clear();

    json_t* data = json_object_get(root.get(), "data");
    json_t* apps = data ? json_object_get(data, "apps") : nullptr;
    json_t* items = apps ? json_object_get(apps, "items") : nullptr;
    if (!json_is_array(items))
        return {};

    json_t* page_info = json_object_get(apps, "pageInfo");
    has_next_page = GetBool(page_info, "hasNextPage");
    end_cursor = GetString(page_info, "endCursor");

    std::vector<PublicGame> games;
    size_t index = 0;
    json_t* app = nullptr;
    json_array_foreach(items, index, app)
    {
        GameInfo parsed = ParseApp(app);
        if (!parsed.id.empty() && !parsed.title.empty())
            games.push_back(ToPublicGame(parsed));
    }
    return games;
}

std::string BuildCatalogRequestBody(
    const std::string& vpc_id, const std::string& search_query,
    const std::string& cursor)
{
    static const char* kBrowseQuery = R"GRAPHQL(
query GetFilterBrowseResults($vpcId: String!, $locale: String!, $sortString: String!, $fetchCount: Int!, $cursor: String!, $filters: AppFilterFields!) {
  apps(vpcId: $vpcId, language: $locale, orderBy: $sortString, first: $fetchCount, after: $cursor, filters: $filters) {
    numberReturned numberSupported
    pageInfo { hasNextPage endCursor totalCount }
    items {
      id title longDescription shortDescription publisherName
      images { KEY_ART GAME_BOX_ART TV_BANNER HERO_IMAGE }
      variants { id appStore gfn { status library { status selected } } }
      gfn { playabilityState minimumMembershipTierLabel }
    }
  }
})GRAPHQL";
    static const char* kSearchQuery = R"GRAPHQL(
query GetSearchFilterResults($vpcId: String!, $locale: String!, $sortString: String!, $fetchCount: Int!, $cursor: String!, $searchString: String!, $filters: AppFilterFields!) {
  apps(vpcId: $vpcId, language: $locale, orderBy: $sortString, first: $fetchCount, after: $cursor, searchQuery: $searchString, filters: $filters) {
    numberReturned numberSupported
    pageInfo { hasNextPage endCursor totalCount }
    items {
      id title longDescription shortDescription publisherName
      images { KEY_ART GAME_BOX_ART TV_BANNER HERO_IMAGE }
      variants { id appStore gfn { status library { status selected } } }
      gfn { playabilityState minimumMembershipTierLabel }
    }
  }
})GRAPHQL";

    JsonPtr body(json_object(), &json_decref);
    JsonPtr variables(json_object(), &json_decref);
    json_object_set_new(body.get(), "query", json_string(search_query.empty() ? kBrowseQuery : kSearchQuery));
    json_object_set_new(variables.get(), "vpcId", json_string(vpc_id.c_str()));
    json_object_set_new(variables.get(), "locale", json_string(kDefaultLocale));
    json_object_set_new(
        variables.get(), "sortString",
        json_string("itemMetadata.relevance:DESC,sortName:ASC"));
    json_object_set_new(variables.get(), "fetchCount", json_integer(120));
    json_object_set_new(variables.get(), "cursor", json_string(cursor.c_str()));
    json_object_set_new(variables.get(), "filters", json_object());
    if (!search_query.empty())
        json_object_set_new(variables.get(), "searchString", json_string(search_query.c_str()));
    json_object_set_new(body.get(), "variables", json_incref(variables.get()));
    return DumpJson(body.get());
}

std::vector<std::string> BuildGraphQlPostHeaders(const std::string& token)
{
    std::vector<std::string> headers = BuildGraphQlHeaders(token);
    for (std::string& header : headers)
    {
        if (header.find("Content-Type:") == 0)
            header = "Content-Type: application/json";
    }
    return headers;
}

std::string InferStore(json_t* item)
{
    const std::string explicit_store = GetString(item, "store");
    if (!explicit_store.empty())
        return explicit_store;

    const std::string publisher = GetString(item, "publisher");
    if (publisher.find("NCSoft") != std::string::npos ||
        publisher.find("ncsoft") != std::string::npos)
    {
        return "NCSoft";
    }

    return "Unknown";
}

std::string BuildSteamImageUrl(const std::string& steam_url)
{
    const std::string marker = "/app/";
    const auto marker_pos    = steam_url.find(marker);
    if (marker_pos == std::string::npos)
        return "";

    const auto id_begin = marker_pos + marker.size();
    const auto id_end   = steam_url.find('/', id_begin);
    const std::string steam_id =
        steam_url.substr(id_begin, id_end == std::string::npos ? std::string::npos : id_end - id_begin);

    if (steam_id.empty())
        return "";

    return "https://cdn.cloudflare.steamstatic.com/steam/apps/" + steam_id + "/library_600x900.jpg";
}

} // namespace

std::vector<LoginProvider> GfnClient::FetchLoginProviders() const
{
    const HttpResponse response = http_client_.Get(
        kServiceUrlsEndpoint,
        kUserAgent,
        {"Accept: application/json"});

    if (response.status_code != 200)
    {
        throw std::runtime_error(
            "Provider discovery failed with HTTP " + std::to_string(response.status_code));
    }

    JsonPtr root = LoadJson(response.body);
    json_t* gfn_service_info =
        json_object_get(root.get(), "gfnServiceInfo");
    json_t* endpoints =
        gfn_service_info ? json_object_get(gfn_service_info, "gfnServiceEndpoints") : nullptr;

    std::vector<LoginProvider> providers;
    if (json_is_array(endpoints))
    {
        size_t index = 0;
        json_t* entry = nullptr;
        json_array_foreach(endpoints, index, entry)
        {
            LoginProvider provider;
            provider.idp_id                = GetString(entry, "idpId");
            provider.code                  = GetString(entry, "loginProviderCode");
            provider.display_name          = GetString(entry, "loginProviderDisplayName");
            provider.streaming_service_url = EnsureTrailingSlash(GetString(entry, "streamingServiceUrl"));
            provider.priority              = GetInteger(entry, "loginProviderPriority", 0);

            if (provider.code == "BPC")
                provider.display_name = "bro.game";

            if (!provider.idp_id.empty() && !provider.streaming_service_url.empty())
                providers.push_back(provider);
        }
    }

    if (providers.empty())
        providers.push_back(DefaultProvider());

    std::sort(
        providers.begin(),
        providers.end(),
        [](const LoginProvider& left, const LoginProvider& right) {
            return left.priority < right.priority;
        });

    return providers;
}

std::vector<PublicGame> GfnClient::FetchPublicGames() const
{
    const HttpResponse response = http_client_.Get(
        kPublicCatalogEndpoint,
        kUserAgent,
        {"Accept: application/json"});

    if (response.status_code != 200)
    {
        throw std::runtime_error(
            "Public catalog fetch failed with HTTP " + std::to_string(response.status_code));
    }

    JsonPtr root = LoadJson(response.body);
    if (!json_is_array(root.get()))
        throw std::runtime_error("Public catalog payload is not a JSON array");

    std::vector<PublicGame> games;

    size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(root.get(), index, item)
    {
        const std::string status = GetString(item, "status");
        const std::string title  = GetString(item, "title");
        if (status != "AVAILABLE" || title.empty())
            continue;

        PublicGame game;
        game.id        = GetString(item, "id");
        game.title     = title;
        game.store     = InferStore(item);
        game.publisher = GetString(item, "publisher");
        game.image_url = BuildSteamImageUrl(GetString(item, "steamUrl"));
        if (game.id.empty())
            game.id = game.title;

        games.push_back(std::move(game));
    }

    std::sort(
        games.begin(),
        games.end(),
        [](const PublicGame& left, const PublicGame& right) {
            return left.title < right.title;
        });

    return games;
}

std::vector<PublicGame> GfnClient::FetchCatalogGames(
    AuthSession& session, const std::string& search_query) const
{
    session = RecoverSavedSession(session);
    std::string jwt_token = ResolveSessionJwt(session);
    const std::string vpc_id = ResolveVpcId(http_client_, session);

    std::vector<PublicGame> games;
    std::unordered_set<std::string> seen_ids;
    std::string cursor;

    for (int page = 0; page < 3; ++page)
    {
        HttpResponse response = http_client_.Post(
            kGraphQlEndpoint,
            kUserAgent,
            BuildGraphQlPostHeaders(jwt_token),
            BuildCatalogRequestBody(vpc_id, search_query, cursor));

        if (response.status_code == 401)
        {
            session = RecoverSavedSession(session, true);
            jwt_token = ResolveSessionJwt(session);
            response = http_client_.Post(
                kGraphQlEndpoint,
                kUserAgent,
                BuildGraphQlPostHeaders(jwt_token),
                BuildCatalogRequestBody(vpc_id, search_query, cursor));
        }

        if (response.status_code != 200)
        {
            throw std::runtime_error(
                "Catalog browse failed with HTTP " + std::to_string(response.status_code));
        }

        JsonPtr root = LoadJson(response.body);
        bool has_next_page = false;
        std::string end_cursor;
        std::vector<PublicGame> page_games =
            ParseCatalogPage(root, has_next_page, end_cursor);
        for (PublicGame& game : page_games)
        {
            const std::string key = game.id.empty() ? game.title : game.id;
            if (seen_ids.insert(key).second)
                games.push_back(std::move(game));
        }

        if (!has_next_page || end_cursor.empty() || end_cursor == cursor)
            break;
        cursor = std::move(end_cursor);
    }

    return games;
}

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
        while (!completed_ && !stop_)
        {
            if (isCancelled && isCancelled())
            {
                Stop();
                return "";
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
    AppendAuthLog("auth: user info ok user_id_present=" + std::string(session.user.user_id.empty() ? "no" : "yes"));
    return session;
}

AuthSession GfnClient::EnsureFreshSession(const AuthSession& session) const
{
    const bool access_needs_refresh = IsNearExpiry(session.tokens.expires_at_ms);
    const bool client_needs_refresh = session.tokens.client_token.empty() ||
        IsNearExpiry(session.tokens.client_token_expires_at_ms);

    if (!access_needs_refresh && !client_needs_refresh)
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
        refreshed.tokens = RefreshTokens(http_client_, session);
        refreshed.last_refresh_at_ms = NowMs();
        AppendAuthLog("auth: access token refresh ok");
    }

    if (refreshed.tokens.client_token.empty() ||
        IsNearExpiry(refreshed.tokens.client_token_expires_at_ms))
    {
        RequestClientToken(http_client_, refreshed.tokens);
    }
    return refreshed;
}

AuthSession GfnClient::EnsureFreshSavedSession(const AuthSession& session) const
{
    if (!session.persistence_enabled)
        return EnsureFreshSession(session);

    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
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
        refreshed.tokens = RefreshTokens(http_client_, session);
        if (refreshed.tokens.client_token.empty() ||
            IsNearExpiry(refreshed.tokens.client_token_expires_at_ms))
        {
            RequestClientToken(http_client_, refreshed.tokens);
        }
        refreshed.last_refresh_at_ms = NowMs();
        return refreshed;
    }

    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
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
    refreshed.tokens = RefreshTokens(http_client_, source);
    if (refreshed.tokens.client_token.empty() ||
        IsNearExpiry(refreshed.tokens.client_token_expires_at_ms))
    {
        RequestClientToken(http_client_, refreshed.tokens);
    }
    refreshed.last_refresh_at_ms = NowMs();
    SaveSession(refreshed);
    return refreshed;
}

bool ParseSessionObject(json_t* root, AuthSession& session)
{
    if (!root || !json_is_object(root))
        return false;

    session.provider.idp_id                = GetString(json_object_get(root, "provider"), "idp_id");
    session.provider.code                  = GetString(json_object_get(root, "provider"), "code");
    session.provider.display_name          = GetString(json_object_get(root, "provider"), "display_name");
    session.provider.streaming_service_url =
        EnsureTrailingSlash(GetString(json_object_get(root, "provider"), "streaming_service_url"));
    session.provider.priority = GetInteger(json_object_get(root, "provider"), "priority", 0);

    json_t* tokens = json_object_get(root, "tokens");
    session.tokens.access_token  = GetString(tokens, "access_token");
    session.tokens.refresh_token = GetString(tokens, "refresh_token");
    session.tokens.id_token      = GetString(tokens, "id_token");
    session.tokens.client_token  = GetString(tokens, "client_token");

    json_t* expires = tokens ? json_object_get(tokens, "expires_at_ms") : nullptr;
    if (json_is_integer(expires))
        session.tokens.expires_at_ms = static_cast<std::int64_t>(json_integer_value(expires));

    json_t* client_expires = tokens ? json_object_get(tokens, "client_token_expires_at_ms") : nullptr;
    if (json_is_integer(client_expires))
    {
        session.tokens.client_token_expires_at_ms =
            static_cast<std::int64_t>(json_integer_value(client_expires));
    }

    json_t* user = json_object_get(root, "user");
    session.user.user_id         = GetString(user, "user_id");
    session.user.display_name    = GetString(user, "display_name");
    session.user.email           = GetString(user, "email");
    session.user.avatar_url      = GetString(user, "avatar_url");
    session.user.membership_tier = GetString(user, "membership_tier");
    if (session.user.membership_tier.empty())
        session.user.membership_tier = "FREE";

    json_t* last_refresh = json_object_get(root, "last_refresh_at_ms");
    if (json_is_integer(last_refresh))
        session.last_refresh_at_ms = static_cast<std::int64_t>(json_integer_value(last_refresh));

    if (session.provider.idp_id.empty())
        session.provider = DefaultProvider();

    return !session.tokens.access_token.empty() && !session.user.user_id.empty();
}

JsonPtr BuildSessionObject(const AuthSession& session)
{
    JsonPtr root(json_object(), &json_decref);
    JsonPtr provider(json_object(), &json_decref);
    JsonPtr tokens(json_object(), &json_decref);
    JsonPtr user(json_object(), &json_decref);

    json_object_set_new(provider.get(), "idp_id", json_string(session.provider.idp_id.c_str()));
    json_object_set_new(provider.get(), "code", json_string(session.provider.code.c_str()));
    json_object_set_new(provider.get(), "display_name", json_string(session.provider.display_name.c_str()));
    json_object_set_new(
        provider.get(),
        "streaming_service_url",
        json_string(session.provider.streaming_service_url.c_str()));
    json_object_set_new(provider.get(), "priority", json_integer(session.provider.priority));

    json_object_set_new(tokens.get(), "access_token", json_string(session.tokens.access_token.c_str()));
    json_object_set_new(tokens.get(), "refresh_token", json_string(session.tokens.refresh_token.c_str()));
    json_object_set_new(tokens.get(), "id_token", json_string(session.tokens.id_token.c_str()));
    json_object_set_new(tokens.get(), "client_token", json_string(session.tokens.client_token.c_str()));
    json_object_set_new(tokens.get(), "expires_at_ms", json_integer(session.tokens.expires_at_ms));
    json_object_set_new(
        tokens.get(),
        "client_token_expires_at_ms",
        json_integer(session.tokens.client_token_expires_at_ms));

    json_object_set_new(user.get(), "user_id", json_string(session.user.user_id.c_str()));
    json_object_set_new(user.get(), "display_name", json_string(session.user.display_name.c_str()));
    json_object_set_new(user.get(), "email", json_string(session.user.email.c_str()));
    json_object_set_new(user.get(), "avatar_url", json_string(session.user.avatar_url.c_str()));
    json_object_set_new(user.get(), "membership_tier", json_string(session.user.membership_tier.c_str()));

    json_object_set_new(root.get(), "provider", json_incref(provider.get()));
    json_object_set_new(root.get(), "tokens", json_incref(tokens.get()));
    json_object_set_new(root.get(), "user", json_incref(user.get()));
    json_object_set_new(root.get(), "last_refresh_at_ms", json_integer(session.last_refresh_at_ms));
    return root;
}

void WriteJsonToFile(const std::string& path, json_t* root)
{
    char* dump = json_dumps(root, JSON_INDENT(2));
    if (!dump)
        throw std::runtime_error("Failed to serialize JSON");

    std::unique_ptr<char, decltype(&std::free)> output(dump, &std::free);
    WriteTextFileAtomically(path, output.get());
}

void RememberActiveCloudSession(
    const AuthSession& auth,
    const std::string& session_id,
    const std::string& launch_app_id)
{
    if (session_id.empty())
        return;

    try
    {
        JsonPtr root(json_object(), &json_decref);
        json_object_set_new(root.get(), "session_id", json_string(session_id.c_str()));
        json_object_set_new(root.get(), "user_id", json_string(auth.user.user_id.c_str()));
        json_object_set_new(root.get(), "launch_app_id", json_string(launch_app_id.c_str()));
        json_object_set_new(root.get(), "created_at_ms", json_integer(NowMs()));
        WriteJsonToFile(GetActiveCloudSessionPath(), root.get());
    }
    catch (const std::exception& e)
    {
        AppendAuthLog("session: unable to persist active session error=" + std::string(e.what()));
    }
}

std::string LoadActiveCloudSessionId(const AuthSession& auth)
{
    const std::string body = ReadTextFile(GetActiveCloudSessionPath());
    if (body.empty())
        return "";

    try
    {
        JsonPtr root = LoadJson(body);
        if (GetString(root.get(), "user_id") != auth.user.user_id)
            return "";
        return GetString(root.get(), "session_id");
    }
    catch (...)
    {
        return "";
    }
}

void ForgetActiveCloudSession(const std::string& session_id)
{
    const std::string body = ReadTextFile(GetActiveCloudSessionPath());
    if (!body.empty())
    {
        try
        {
            JsonPtr root = LoadJson(body);
            if (!session_id.empty() && GetString(root.get(), "session_id") != session_id)
                return;
        }
        catch (...) {}
    }

    std::remove(GetActiveCloudSessionPath().c_str());
    std::remove((GetActiveCloudSessionPath() + ".bak").c_str());
}

std::vector<AuthSession> LoadAccountsFromDisk(std::string* active_user_id = nullptr)
{
    for (const std::string& path : {GetAccountsPath(), GetAccountsPath() + ".bak"})
    {
        const std::string stored = ReadTextFile(path);
        if (stored.empty())
            continue;

        try
        {
            const std::string body = DecryptTokenVault(stored);
            JsonPtr root = LoadJson(body);
            if (!json_is_object(root.get()))
                continue;

            json_t* accounts = json_object_get(root.get(), "accounts");
            if (!accounts || !json_is_array(accounts))
                continue;

            std::vector<AuthSession> sessions;
            size_t index = 0;
            json_t* item = nullptr;
            json_array_foreach(accounts, index, item)
            {
                AuthSession session;
                if (ParseSessionObject(item, session))
                    sessions.push_back(std::move(session));
            }

            if (sessions.empty())
                continue;

            if (active_user_id)
                *active_user_id = GetString(root.get(), "active_user_id");
            if (path.find(".bak") != std::string::npos)
                AppendAuthLog("auth: recovered accounts from backup");
            else if (stored.rfind(kTokenVaultHeader, 0) != 0)
            {
                AppendAuthLog("auth: loaded legacy plaintext account store; migration scheduled");
                std::string active = GetString(root.get(), "active_user_id");
                if (active.empty())
                    active = sessions.front().user.user_id;
                SaveAccountsToDisk(sessions, active);
                AppendAuthLog("auth: legacy account store migrated to encrypted vault");
            }
            return sessions;
        }
        catch (const std::exception& e)
        {
            AppendAuthLog("auth: account store parse failed path=" + path + " error=" + e.what());
        }
    }

    return {};
}

void SaveAccountsToDisk(const std::vector<AuthSession>& sessions, const std::string& active_user_id)
{
    EnsureAppHome();

    JsonPtr root(json_object(), &json_decref);
    JsonPtr accounts(json_array(), &json_decref);

    for (const AuthSession& session : sessions)
    {
        JsonPtr item = BuildSessionObject(session);
        json_array_append_new(accounts.get(), json_incref(item.get()));
    }

    json_object_set_new(root.get(), "schema_version", json_integer(3));
    json_object_set_new(root.get(), "active_user_id", json_string(active_user_id.c_str()));
    json_object_set_new(root.get(), "accounts", json_incref(accounts.get()));
    char* dump = json_dumps(root.get(), JSON_COMPACT | JSON_SORT_KEYS);
    if (!dump)
        throw std::runtime_error("Failed to serialize token vault");
    std::unique_ptr<char, decltype(&std::free)> plaintext(dump, &std::free);
    const std::string encrypted = EncryptTokenVault(plaintext.get());
    if (DecryptTokenVault(encrypted) != plaintext.get())
        throw std::runtime_error("Token vault round-trip verification failed");
    WriteTextFileAtomically(GetAccountsPath(), encrypted);

    const std::string backup_path = GetAccountsPath() + ".bak";
    const std::string backup = ReadTextFile(backup_path);
    if (!backup.empty() && backup.rfind(kTokenVaultHeader, 0) != 0)
        std::remove(backup_path.c_str());

    // The encrypted multi-account vault replaces the old plaintext session copy.
    std::remove(GetSessionPath().c_str());
    std::remove((GetSessionPath() + ".bak").c_str());
    AppendAuthLog("auth: token vault saved accounts=" + std::to_string(sessions.size()) +
                  " encrypted=1 roundTrip=ok");
}

std::vector<NativeCredentials> LoadNativeCredentialEntries()
{
    const std::string stored = ReadTextFile(GetNativeCredentialsPath());
    if (stored.empty())
        return {};
    try
    {
        JsonPtr root = LoadJson(DecryptTokenVault(stored));
        json_t* entries = json_object_get(root.get(), "entries");
        if (!json_is_array(entries))
            return {};
        std::vector<NativeCredentials> result;
        size_t index = 0;
        json_t* item = nullptr;
        json_array_foreach(entries, index, item)
        {
            NativeCredentials credentials;
            credentials.provider_id = GetString(item, "provider_id");
            credentials.email = GetString(item, "email");
            credentials.password = GetString(item, "password");
            if (!credentials.provider_id.empty() && !credentials.email.empty() &&
                !credentials.password.empty())
            {
                result.push_back(std::move(credentials));
            }
        }
        return result;
    }
    catch (const std::exception& e)
    {
        AppendAuthLog(std::string("auth-native: credential vault load failed error=") + e.what());
        return {};
    }
}

void SaveNativeCredentialEntries(const std::vector<NativeCredentials>& entries)
{
    EnsureAppHome();
    if (entries.empty())
    {
        std::remove(GetNativeCredentialsPath().c_str());
        std::remove((GetNativeCredentialsPath() + ".bak").c_str());
        return;
    }

    JsonPtr root(json_object(), &json_decref);
    JsonPtr values(json_array(), &json_decref);
    for (const NativeCredentials& credentials : entries)
    {
        JsonPtr item(json_object(), &json_decref);
        json_object_set_new(item.get(), "provider_id", json_string(credentials.provider_id.c_str()));
        json_object_set_new(item.get(), "email", json_string(credentials.email.c_str()));
        json_object_set_new(item.get(), "password", json_string(credentials.password.c_str()));
        json_array_append_new(values.get(), json_incref(item.get()));
    }
    json_object_set_new(root.get(), "schema_version", json_integer(1));
    json_object_set_new(root.get(), "entries", json_incref(values.get()));
    char* dump = json_dumps(root.get(), JSON_COMPACT | JSON_SORT_KEYS);
    if (!dump)
        throw std::runtime_error("Failed to serialize the credential vault");
    std::unique_ptr<char, decltype(&std::free)> plaintext(dump, &std::free);
    const std::string encrypted = EncryptTokenVault(plaintext.get());
    if (DecryptTokenVault(encrypted) != plaintext.get())
        throw std::runtime_error("Credential vault round-trip verification failed");
    WriteTextFileAtomically(GetNativeCredentialsPath(), encrypted);
    AppendAuthLog("auth-native: credential vault saved entries=" + std::to_string(entries.size()));
}

bool LoadLegacySession(AuthSession& session)
{
    for (const std::string& path : {GetSessionPath(), GetSessionPath() + ".bak"})
    {
        const std::string body = ReadTextFile(path);
        if (body.empty())
            continue;
        try
        {
            JsonPtr root = LoadJson(body);
            if (ParseSessionObject(root.get(), session))
                return true;
        }
        catch (...) {}
    }

    return false;
}

std::vector<GameInfo> GfnClient::FetchLibraryGames(AuthSession& session) const
{
    session = RecoverSavedSession(session);
    std::string jwt_token = ResolveSessionJwt(session);
    const std::string vpc_id    = ResolveVpcId(http_client_, session);

    HttpResponse response = http_client_.Get(
        BuildLibraryUrl(vpc_id, true),
        kUserAgent,
        BuildGraphQlHeaders(jwt_token));

    if (response.status_code == 401)
    {
        session = RecoverSavedSession(session, true);
        jwt_token = ResolveSessionJwt(session);
        response = http_client_.Get(
            BuildLibraryUrl(vpc_id, true),
            kUserAgent,
            BuildGraphQlHeaders(jwt_token));
    }

    if (response.status_code != 200)
    {
        response = http_client_.Get(
            BuildLibraryUrl(vpc_id, false),
            kUserAgent,
            BuildGraphQlHeaders(jwt_token));
    }

    if (response.status_code != 200)
    {
        throw std::runtime_error(
            "Library fetch failed with HTTP " + std::to_string(response.status_code));
    }

    JsonPtr root = LoadJson(response.body);
    std::vector<GameInfo> games = ParseLibraryGames(root);
    ApplyPlayHistory(games, LoadPlayHistory());
    return games;
}

bool GfnClient::LoadSavedSession(AuthSession& session) const
{
    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
    std::string active_user_id;
    std::vector<AuthSession> sessions = LoadAccountsFromDisk(&active_user_id);
    if (!sessions.empty())
    {
        for (const AuthSession& saved : sessions)
        {
            if (saved.user.user_id == active_user_id)
            {
                session = saved;
                AppendAuthLog("auth: selected saved account " + AuthSessionHealth(session));
                return true;
            }
        }

        session = sessions.front();
        SaveAccountsToDisk(sessions, session.user.user_id);
        AppendAuthLog("auth: selected fallback account " + AuthSessionHealth(session));
        return true;
    }

    if (LoadLegacySession(session))
    {
        SaveSession(session);
        AppendAuthLog("auth: selected migrated legacy account " + AuthSessionHealth(session));
        return true;
    }

    return false;
}

std::vector<AuthSession> GfnClient::LoadSavedSessions() const
{
    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
    std::vector<AuthSession> sessions = LoadAccountsFromDisk();
    if (!sessions.empty())
        return sessions;

    AuthSession legacy;
    if (LoadLegacySession(legacy))
    {
        SaveSession(legacy);
        sessions.push_back(std::move(legacy));
    }

    return sessions;
}

void GfnClient::SaveSession(const AuthSession& session) const
{
    if (!session.persistence_enabled)
        return;

    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
    EnsureAppHome();

    std::vector<AuthSession> sessions = LoadAccountsFromDisk();
    bool replaced = false;
    for (AuthSession& saved : sessions)
    {
        if (saved.user.user_id == session.user.user_id)
        {
            saved = session;
            replaced = true;
            break;
        }
    }

    if (!replaced)
        sessions.push_back(session);

    SaveAccountsToDisk(sessions, session.user.user_id);
}

bool GfnClient::SetActiveSavedSession(const std::string& user_id) const
{
    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
    std::vector<AuthSession> sessions = LoadAccountsFromDisk();
    for (const AuthSession& session : sessions)
    {
        if (session.user.user_id == user_id)
        {
            SaveAccountsToDisk(sessions, session.user.user_id);
            return true;
        }
    }

    return false;
}

void GfnClient::ClearSavedSession() const
{
    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
    AuthSession active;
    if (!LoadSavedSession(active))
    {
        std::remove(GetSessionPath().c_str());
        std::remove((GetSessionPath() + ".bak").c_str());
        return;
    }

    std::vector<AuthSession> sessions = LoadAccountsFromDisk();
    sessions.erase(
        std::remove_if(
            sessions.begin(),
            sessions.end(),
            [&active](const AuthSession& session) {
                return session.user.user_id == active.user.user_id;
            }),
        sessions.end());

    if (sessions.empty())
    {
        std::remove(GetAccountsPath().c_str());
        std::remove((GetAccountsPath() + ".bak").c_str());
        std::remove(GetSessionPath().c_str());
        std::remove((GetSessionPath() + ".bak").c_str());
        return;
    }

    SaveAccountsToDisk(sessions, sessions.front().user.user_id);
}

void GfnClient::ClearAllSavedSessions() const
{
    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
    std::remove(GetAccountsPath().c_str());
    std::remove((GetAccountsPath() + ".bak").c_str());
    std::remove(GetSessionPath().c_str());
    std::remove((GetSessionPath() + ".bak").c_str());
}

std::optional<NativeCredentials> GfnClient::LoadNativeCredentials(
    const std::string& provider_id) const
{
    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
    for (NativeCredentials& credentials : LoadNativeCredentialEntries())
    {
        if (credentials.provider_id == provider_id)
            return credentials;
    }
    return std::nullopt;
}

void GfnClient::SaveNativeCredentials(const NativeCredentials& credentials) const
{
    if (credentials.provider_id.empty() || credentials.email.empty() || credentials.password.empty())
        throw std::runtime_error("Cannot save incomplete NVIDIA credentials");
    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
    std::vector<NativeCredentials> entries = LoadNativeCredentialEntries();
    bool replaced = false;
    for (NativeCredentials& saved : entries)
    {
        if (saved.provider_id == credentials.provider_id)
        {
            saved = credentials;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        entries.push_back(credentials);
    SaveNativeCredentialEntries(entries);
}

void GfnClient::ClearNativeCredentials(const std::string& provider_id) const
{
    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
    std::vector<NativeCredentials> entries = LoadNativeCredentialEntries();
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const NativeCredentials& entry) {
            return entry.provider_id == provider_id;
        }),
        entries.end());
    SaveNativeCredentialEntries(entries);
    AppendAuthLog("auth-native: saved password removed");
}

void GfnClient::ClearAllNativeCredentials() const
{
    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
    SaveNativeCredentialEntries({});
    AppendAuthLog("auth-native: all saved passwords removed");
}

std::string GfnClient::LoadLauncherPreference(
    const std::string& user_id, const std::string& game_id) const
{
    if (user_id.empty() || game_id.empty())
        return "";
    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
    try {
        const std::string stored = ReadTextFile(GetLauncherPreferencesPath());
        if (stored.empty())
            return "";
        JsonPtr root = LoadJson(stored);
        return GetString(root.get(), (user_id + ":" + game_id).c_str());
    } catch (const std::exception& e) {
        AppendAuthLog("launcher: preference load failed error=" + std::string(e.what()));
        return "";
    }
}

void GfnClient::SaveLauncherPreference(
    const std::string& user_id, const std::string& game_id,
    const std::string& variant_id) const
{
    if (user_id.empty() || game_id.empty() || variant_id.empty())
        return;
    std::lock_guard<std::recursive_mutex> lock(g_accounts_mutex);
    try {
        JsonPtr root(json_object(), &json_decref);
        const std::string stored = ReadTextFile(GetLauncherPreferencesPath());
        if (!stored.empty()) {
            try {
                JsonPtr loaded = LoadJson(stored);
                if (json_is_object(loaded.get()))
                    root = std::move(loaded);
            } catch (...) {
            }
        }
        json_object_set_new(root.get(), (user_id + ":" + game_id).c_str(),
                            json_string(variant_id.c_str()));
        WriteJsonToFile(GetLauncherPreferencesPath(), root.get());
        AppendAuthLog("launcher: preference saved game=" + game_id + " variant=" + variant_id);
    } catch (const std::exception& e) {
        AppendAuthLog("launcher: preference save failed error=" + std::string(e.what()));
    }
}

[[maybe_unused]] static std::string GenerateUUID()
{
    auto bytes = GenerateRandomBytes(16);
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    std::string hex = HexEncode(bytes.data(), 16);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20);
}

static std::string BuildSessionBody(
    const std::string& app_id,
    const std::string& internal_title,
    const std::string& device_id,
    const StreamSettings& stream_settings)
{
    json_t* root = json_object();
    json_t* req = json_object();
    try {
        json_object_set_new(req, "appId", json_integer(std::stoll(app_id)));
    } catch (...) {
        json_object_set_new(req, "appId", json_integer(0));
    }
    json_object_set_new(req, "cmsId", json_string(app_id.c_str()));
    if (internal_title.empty())
        json_object_set_new(req, "internalTitle", json_null());
    else
        json_object_set_new(req, "internalTitle", json_string(internal_title.c_str()));
    json_object_set_new(req, "networkTestSessionId", json_null());
    json_object_set_new(req, "parentSessionId", json_null());
    json_object_set_new(req, "clientIdentification", json_string("GFN-PC"));
    json_object_set_new(req, "deviceHashId", json_string(device_id.c_str()));
    json_object_set_new(req, "clientVersion", json_string("30.0"));
    json_object_set_new(req, "clientPlatformName", json_string("windows"));

    json_object_set_new(req, "availableSupportedControllers", json_array());
    json_object_set_new(req, "sdkVersion", json_string("1.0"));
    json_object_set_new(req, "streamerVersion", json_integer(1));
    json_object_set_new(req, "useOps", json_true());
    json_object_set_new(req, "audioMode", json_integer(2));
    json_object_set_new(req, "sdrHdrMode", json_integer(0));
    json_object_set_new(req, "clientDisplayHdrCapabilities", json_null());
    json_object_set_new(req, "surroundAudioInfo", json_integer(0));
    // Reserve player 1 for the virtual XInput controller exposed by the
    // Switch stream view. The gamepad reports use the same active bitmap.
    json_object_set_new(req, "remoteControllersBitmap", json_integer(1));
    json_object_set_new(req, "enhancedStreamMode", json_integer(1));
    json_object_set_new(req, "appLaunchMode", json_integer(1));
    json_object_set_new(req, "secureRTSPSupported", json_false());
    json_object_set_new(req, "partnerCustomData", json_string(""));
    json_object_set_new(req, "accountLinked", json_true());
    json_object_set_new(
        req, "enablePersistingInGameSettings",
        json_boolean(stream_settings.persist_game_settings));
    json_object_set_new(req, "userAge", json_integer(26));
    json_object_set_new(req, "clientTimezoneOffset", json_integer(0));
    
    json_t* features = json_object();
    json_object_set_new(features, "reflex", json_false());
    // CloudMatch uses an enum here: 0 = 8-bit SDR, 10 = 10-bit capable.
    // Sending literal 8 makes the whole launch request fail with a generic
    // INTERNAL_ERROR_STATUS before the session can enter queue/setup.
    json_object_set_new(features, "bitDepth", json_integer(0));
    json_object_set_new(features, "cloudGsync", json_false());
    json_object_set_new(features, "enabledL4S", json_false());
    json_object_set_new(features, "mouseMovementFlags", json_integer(0));
    json_object_set_new(features, "trueHdr", json_false());
    json_object_set_new(features, "supportedHidDevices", json_integer(0));
    json_object_set_new(features, "profile", json_integer(0));
    json_object_set_new(features, "fallbackToLogicalResolution", json_false());
    json_object_set_new(features, "hidDevices", json_null());
    json_object_set_new(features, "chromaFormat", json_integer(0));
    json_object_set_new(features, "prefilterMode", json_integer(0));
    json_object_set_new(features, "prefilterSharpness", json_integer(0));
    json_object_set_new(features, "prefilterNoiseReduction", json_integer(0));
    json_object_set_new(features, "hudStreamingMode", json_integer(0));
    json_object_set_new(features, "sdrColorSpace", json_integer(2));
    json_object_set_new(features, "hdrColorSpace", json_integer(0));
    json_object_set_new(req, "requestedStreamingFeatures", features);

    json_t* meta = json_array();
    json_t* m1 = json_object(); json_object_set_new(m1, "key", json_string("SubSessionId")); json_object_set_new(m1, "value", json_string(device_id.c_str())); json_array_append_new(meta, m1);
    json_t* m2 = json_object(); json_object_set_new(m2, "key", json_string("wssignaling")); json_object_set_new(m2, "value", json_string("1")); json_array_append_new(meta, m2);
    json_t* m3 = json_object(); json_object_set_new(m3, "key", json_string("GSStreamerType")); json_object_set_new(m3, "value", json_string("WebRTC")); json_array_append_new(meta, m3);
    json_object_set_new(req, "metaData", meta);

    json_t* monitors = json_array();
    json_t* monitor = json_object();
    json_object_set_new(monitor, "monitorId", json_integer(0));
    json_object_set_new(monitor, "positionX", json_integer(0));
    json_object_set_new(monitor, "positionY", json_integer(0));
    json_object_set_new(monitor, "widthInPixels", json_integer(stream_settings.width));
    json_object_set_new(monitor, "heightInPixels", json_integer(stream_settings.height));
    json_object_set_new(monitor, "framesPerSecond", json_integer(stream_settings.fps));
    json_object_set_new(monitor, "sdrHdrMode", json_integer(0));
    json_object_set_new(monitor, "displayData", json_null());
    json_object_set_new(monitor, "hdr10PlusGamingData", json_null());
    json_object_set_new(monitor, "dpi", json_integer(100));
    json_array_append_new(monitors, monitor);
    json_object_set_new(req, "clientRequestMonitorSettings", monitors);

    json_object_set_new(root, "sessionRequestData", req);

    char* dump = json_dumps(root, 0);
    std::string body(dump);
    free(dump);
    json_decref(root);
    return body;
}

int ParseSessionStatus(json_t* status)
{
    if (json_is_integer(status))
        return static_cast<int>(json_integer_value(status));

    if (!json_is_string(status))
        return -1;

    const std::string value = Lowercase(JsonString(status));
    if (value == "queued")
        return 0;

    if (value == "provisioning" || value == "initializing" || value == "setup" ||
        value == "setting_up" || value == "launching" || value == "launching_game")
        return 1;

    if (value == "active" || value == "ready" || value == "paused")
        return 2;

    if (value == "streaming" || value == "playing" || value == "connected")
        return 3;

    if (value.find("ad") != std::string::npos)
        return 6;

    if (value.find("fail") != std::string::npos || value.find("error") != std::string::npos ||
        value.find("closed") != std::string::npos || value.find("terminated") != std::string::npos ||
        value.find("cancel") != std::string::npos)
        return 4;

    return -1;
}

std::string SessionInfoTraceSummary(const SessionInfo& info)
{
    std::ostringstream out;
    out << "sessionId=" << info.session_id
        << " status=" << info.status
        << " queuePosition=" << info.queue_position
        << " appPatching=" << (info.app_patching ? 1 : 0)
        << " serverIp=" << info.server_ip
        << " signalingUrl=" << info.signaling_url
        << " media=" << info.media_ip << ":" << info.media_port
        << " iceServers=" << info.ice_servers.size()
        << " sessionToken=" << (info.session_token.empty() ? "missing" : "present");
    return out.str();
}

SessionInfo GfnClient::StartSession(AuthSession& session, const std::string& launch_app_id,
                                    const std::string& launch_store,
                                    const std::string& internal_title) const
{
    session = EnsureFreshSavedSession(session);
    std::string jwt_token = ResolveSessionJwt(session);
    const std::string device_id = device_id_;

    const StreamSettings stream_settings = LoadStreamSettings();
    std::string url = session.provider.streaming_service_url +
        "v2/session?keyboardLayout=en-US_qwerty&languageCode=" +
        stream_settings.game_language;

    std::vector<std::string> headers = {
        "Authorization: GFNJWT " + jwt_token,
        "Content-Type: application/json",
        "nv-client-id: " + client_id_,
        "nv-browser-type: CHROME",
        "nv-client-streamer: NVIDIA-CLASSIC",
        "nv-client-type: NATIVE",
        "nv-client-version: 30.0",
        "nv-device-make: UNKNOWN",
        "nv-device-model: UNKNOWN",
        "nv-device-os: WINDOWS",
        "nv-device-type: DESKTOP",
        "x-device-id: " + device_id,
        "Origin: https://play.geforcenow.com",
        "Referer: https://play.geforcenow.com/"
    };

    std::string body = BuildSessionBody(launch_app_id, internal_title, device_id, stream_settings);
    ResetSessionTraceLog("StartSession appId=" + launch_app_id +
                         " store=" + (launch_store.empty() ? "unknown" : launch_store) +
                         " internalTitle=" + (internal_title.empty() ? "missing" : internal_title));
    AppendSessionTraceLog("START url=" + url);
    AppendSessionTraceLog(
        "START settings=" + std::to_string(stream_settings.width) + "x" +
        std::to_string(stream_settings.height) + "@" + std::to_string(stream_settings.fps) +
        " bitrate=" + std::to_string(stream_settings.bitrate_kbps) +
        "kbps language=" + stream_settings.game_language +
        " persistGameSettings=" +
        std::to_string(stream_settings.persist_game_settings ? 1 : 0) +
        " deviceId=" + device_id);
    AppendSessionTraceLog("START headers:\n" + HeadersForTrace(headers));
    AppendSessionTraceLog("START request body:\n" + JsonForTrace(body));

    HttpResponse response = http_client_.Post(url, kUserAgent, headers, body);
    if (response.status_code == 401)
    {
        session = ForceRefreshSavedSession(session);
        jwt_token = ResolveSessionJwt(session);
        headers[0] = "Authorization: GFNJWT " + jwt_token;
        AppendSessionTraceLog("START authorization rejected; refreshed token and retrying once");
        response = http_client_.Post(url, kUserAgent, headers, body);
    }
    AppendSessionTraceLog("START response HTTP " + std::to_string(response.status_code));
    AppendSessionTraceLog("START response body:\n" + JsonForTrace(response.body));

    JsonPtr root(nullptr, &json_decref);
    try {
        root = LoadJson(response.body);
    } catch (const std::exception& e) {
        AppendSessionTraceLog(std::string("START JSON parse error: ") + e.what());
    }

    const bool app_patching = root && IsAppPatchingResponse(root.get());
    if (root)
    {
        json_t* req_status = json_object_get(root.get(), "requestStatus");
        if (req_status && GetInteger(req_status, "statusCode") != 1 && !app_patching)
        {
            const std::string error = BuildCloudMatchErrorMessage("StartSession", response.status_code, root.get());
            AppendSessionTraceLog("START api error: " + error);
            throw std::runtime_error(error);
        }
    }

    if (response.status_code != 200 && response.status_code != 202 && !app_patching)
    {
        const std::string error = BuildCloudMatchErrorMessage("StartSession", response.status_code, root.get());
        AppendSessionTraceLog("START http error: " + error);
        throw std::runtime_error(error);
    }

    if (!root) {
        AppendSessionTraceLog("START failed: empty or invalid JSON");
        throw std::runtime_error("StartSession failed: Empty or invalid JSON" + SessionTraceHint());
    }

    SessionInfo info;
    info.app_patching = app_patching;
    CollectIceServersFromObject(root.get(), info);
    
    json_t* sess = json_object_get(root.get(), "session");
    if (sess)
    {
        json_t* s_id = json_object_get(sess, "sessionId");
        if (s_id && json_is_string(s_id))
            info.session_id = json_string_value(s_id);
            
        json_t* status = json_object_get(sess, "status");
        if (status)
            info.status = ParseSessionStatus(status);
            
        json_t* qp = json_object_get(sess, "queuePosition");
        if (qp && json_is_integer(qp))
            info.queue_position = json_integer_value(qp);

        ApplySeatSetupInfo(sess, info);

        ApplySessionNetworkInfo(sess, info);
    }
    
    // Check root for queuePosition
    json_t* root_qp = json_object_get(root.get(), "queuePosition");
    if (root_qp && json_is_integer(root_qp)) {
        info.queue_position = json_integer_value(root_qp);
    }

    if (info.session_id.empty()) {
        AppendSessionTraceLog("START failed: session ID missing after successful response");
        throw std::runtime_error("Failed to parse session ID from response" + SessionTraceHint());
    }

    if (app_patching)
        AppendSessionTraceLog("START pending reason=app_patching; polling created session");
    RememberActiveCloudSession(session, info.session_id, launch_app_id);
    AppendSessionTraceLog("START parsed: " + SessionInfoTraceSummary(info));
    return info;
}

SessionInfo GfnClient::PollSession(AuthSession& session, const std::string& session_id) const
{
    session = EnsureFreshSavedSession(session);
    std::string jwt_token = ResolveSessionJwt(session);
    const std::string device_id = device_id_;
    std::string url = session.provider.streaming_service_url + "v2/session/" + session_id;

    std::vector<std::string> headers = {
        "Authorization: GFNJWT " + jwt_token,
        "nv-client-id: " + client_id_,
        "nv-browser-type: CHROME",
        "nv-client-streamer: NVIDIA-CLASSIC",
        "nv-client-type: NATIVE",
        "nv-client-version: 30.0",
        "nv-device-make: UNKNOWN",
        "nv-device-model: UNKNOWN",
        "nv-device-os: WINDOWS",
        "nv-device-type: DESKTOP",
        "x-device-id: " + device_id,
        "Content-Type: application/json"
    };

    AppendSessionTraceLog("POLL url=" + url);
    AppendSessionTraceLog("POLL headers:\n" + HeadersForTrace(headers));
    HttpResponse response = http_client_.Get(url, kUserAgent, headers);
    if (response.status_code == 401)
    {
        session = ForceRefreshSavedSession(session);
        jwt_token = ResolveSessionJwt(session);
        headers[0] = "Authorization: GFNJWT " + jwt_token;
        AppendSessionTraceLog("POLL authorization rejected; refreshed token and retrying once");
        response = http_client_.Get(url, kUserAgent, headers);
    }
    AppendSessionTraceLog("POLL response HTTP " + std::to_string(response.status_code));
    AppendSessionTraceLog("POLL response body:\n" + JsonForTrace(response.body));

    JsonPtr root(nullptr, &json_decref);
    try {
        root = LoadJson(response.body);
    } catch (const std::exception& e) {
        AppendSessionTraceLog(std::string("POLL JSON parse error: ") + e.what());
    }

    const bool app_patching = root && IsAppPatchingResponse(root.get());
    if (root)
    {
        json_t* req_status = json_object_get(root.get(), "requestStatus");
        if (req_status && GetInteger(req_status, "statusCode") != 1 && !app_patching)
        {
            const std::string error = BuildCloudMatchErrorMessage("PollSession", response.status_code, root.get());
            AppendSessionTraceLog("POLL api error: " + error);
            throw std::runtime_error(error);
        }
    }

    if (response.status_code != 200 && !app_patching)
    {
        const std::string error = BuildCloudMatchErrorMessage("PollSession", response.status_code, root.get());
        AppendSessionTraceLog("POLL http error: " + error);
        throw std::runtime_error(error);
    }

    if (!root) {
        AppendSessionTraceLog("POLL failed: empty or invalid JSON");
        throw std::runtime_error("PollSession failed: Empty or invalid JSON" + SessionTraceHint());
    }

    // Keep diagnostics useful without persisting session tokens returned by CloudMatch.
    const std::string redacted_session = JsonForTrace(response.body);
    FILE* session_dump = fopen("sdmc:/switch/SwitchNOW/switchnow_session.json", "w");
    if (session_dump) {
        fputs(redacted_session.c_str(), session_dump);
        fclose(session_dump);
    }

    SessionInfo info;
    info.session_id = session_id;
    info.app_patching = app_patching;
    CollectIceServersFromObject(root.get(), info);
    
    json_t* sess = json_object_get(root.get(), "session");
    if (sess)
    {
        json_t* status = json_object_get(sess, "status");
        if (status)
            info.status = ParseSessionStatus(status);
            
        json_t* qp = json_object_get(sess, "queuePosition");
        if (qp && json_is_integer(qp))
            info.queue_position = json_integer_value(qp);

        ApplySeatSetupInfo(sess, info);
            
        ApplySessionNetworkInfo(sess, info);
    }
    
    // Sometimes queuePosition is at the root level!
    json_t* root_qp = json_object_get(root.get(), "queuePosition");
    if (root_qp && json_is_integer(root_qp)) {
        info.queue_position = json_integer_value(root_qp);
    }

    if (app_patching)
        AppendSessionTraceLog("POLL pending reason=app_patching");
    AppendSessionTraceLog("POLL parsed: " + SessionInfoTraceSummary(info));
    return info;
}

void GfnClient::StopSession(AuthSession& session, const std::string& session_id) const
{
    session = EnsureFreshSavedSession(session);
    std::string jwt_token = ResolveSessionJwt(session);
    const std::string device_id = device_id_;
    std::string url = session.provider.streaming_service_url + "v2/session/" + session_id;

    std::vector<std::string> headers = {
        "Authorization: GFNJWT " + jwt_token,
        "nv-client-id: " + client_id_,
        "nv-browser-type: CHROME",
        "nv-client-streamer: NVIDIA-CLASSIC",
        "nv-client-type: NATIVE",
        "nv-client-version: 30.0",
        "nv-device-make: UNKNOWN",
        "nv-device-model: UNKNOWN",
        "nv-device-os: WINDOWS",
        "nv-device-type: DESKTOP",
        "x-device-id: " + device_id,
        "Content-Type: application/json"
    };

    AppendSessionTraceLog("STOP url=" + url);
    AppendSessionTraceLog("STOP headers:\n" + HeadersForTrace(headers));
    HttpResponse response = http_client_.Request("DELETE", url, kUserAgent, headers);
    if (response.status_code == 401)
    {
        session = ForceRefreshSavedSession(session);
        jwt_token = ResolveSessionJwt(session);
        headers[0] = "Authorization: GFNJWT " + jwt_token;
        AppendSessionTraceLog("STOP authorization rejected; refreshed token and retrying once");
        response = http_client_.Request("DELETE", url, kUserAgent, headers);
    }
    AppendSessionTraceLog("STOP response HTTP " + std::to_string(response.status_code));
    AppendSessionTraceLog("STOP response body:\n" + JsonForTrace(response.body));
    if ((response.status_code >= 200 && response.status_code < 300) || response.status_code == 404)
    {
        ForgetActiveCloudSession(session_id);
        return;
    }

    throw std::runtime_error("StopSession failed with HTTP " + std::to_string(response.status_code));
}

void GfnClient::CleanupStaleCloudSession(AuthSession& session) const
{
    const std::string stale_session_id = LoadActiveCloudSessionId(session);
    if (stale_session_id.empty())
        return;

    AppendAuthLog("session: cleaning stale CloudMatch session before launch");
    StopSession(session, stale_session_id);
}

} // namespace opennow
