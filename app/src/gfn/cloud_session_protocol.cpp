#include "cloud_session_internal.hpp"
#include "app_launch_mode_policy.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace opennow
{
using namespace gfn::detail;

namespace gfn::cloud_session
{
namespace
{

bool HasPrefix(const std::string& value, const char* prefix)
{
    return value.rfind(prefix, 0) == 0;
}

bool ValidHost(const std::string& host)
{
    if (host.empty() || host.size() > 253)
        return false;
    std::array<unsigned char, 16> ipv6 {};
    if (host.find(':') != std::string::npos) {
        return inet_pton(AF_INET6, host.c_str(), ipv6.data()) == 1 &&
            std::any_of(ipv6.begin(), ipv6.end(), [](unsigned char byte) { return byte != 0; });
    }
    in_addr ipv4 {};
    if (inet_pton(AF_INET, host.c_str(), &ipv4) == 1)
        return ipv4.s_addr != 0;
    if (std::all_of(host.begin(), host.end(), [](unsigned char ch) { return std::isdigit(ch) || ch == '.'; }))
        return false;
    size_t start = 0;
    while (start < host.size()) {
        const size_t end = host.find('.', start);
        const std::string label = host.substr(start, end - start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
            !std::all_of(label.begin(), label.end(), [](unsigned char ch) {
                return ch < 128 && (std::isalnum(ch) || ch == '-');
            }))
            return false;
        if (end == std::string::npos)
            return true;
        start = end + 1;
    }
    return false;
}

struct Endpoint
{
    std::string host;
    int port = 0;
};

Endpoint ParseEndpoint(const std::string& value)
{
    const size_t scheme = value.find("://");
    if (scheme == std::string::npos || value.find_first_of("@#\\") != std::string::npos ||
        std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch <= 0x20 || ch >= 0x7f; }))
        return {};
    const size_t start = scheme + 3;
    const size_t end = value.find_first_of("/?", start);
    const std::string authority = value.substr(start, end - start);
    std::string host;
    size_t port_start = std::string::npos;
    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos)
            return {};
        host = authority.substr(1, close - 1);
        if (host.find(':') == std::string::npos)
            return {};
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':')
                return {};
            port_start = close + 2;
        }
    } else {
        const size_t colon = authority.find(':');
        host = authority.substr(0, colon);
        if (colon != std::string::npos)
            port_start = colon + 1;
    }
    if (!ValidHost(host))
        return {};
    int port = 443;
    if (port_start != std::string::npos) {
        const char* first = authority.data() + port_start;
        const char* last = authority.data() + authority.size();
        const auto parsed = std::from_chars(first, last, port);
        if (parsed.ec != std::errc() || parsed.ptr != last || port <= 0 || port > 65535)
            return {};
    }
    return {host, port};
}

std::string ValidSignalingUrl(std::string value)
{
    if (!HasPrefix(value, "wss://") && !HasPrefix(value, "https://"))
        return {};
    if (ParseEndpoint(value).host.empty())
        return {};
    if (HasPrefix(value, "https://"))
        value.replace(0, 5, "wss");
    return value;
}

bool IsNativeConnection(json_t* connection)
{
    const int protocol = WireInteger(json_object_get(connection, "appLevelProtocol"));
    const std::string resource = GetString(connection, "resourcePath");
    return protocol == 1 || protocol == 6 || WireInteger(json_object_get(connection, "usage")) == 16 ||
        HasPrefix(resource, "rtsp://") || HasPrefix(resource, "rtsps://");
}

std::string GetConnectionIp(json_t* connection)
{
    if (!connection || !json_is_object(connection))
        return {};

    json_t* ip = json_object_get(connection, "ip");
    if (json_is_string(ip) && ValidHost(JsonString(ip)))
        return JsonString(ip);

    if (json_is_array(ip)) {
        size_t index;
        json_t* entry;
        json_array_foreach(ip, index, entry) {
            if (ValidHost(JsonString(entry)))
                return JsonString(entry);
        }
    }

    const std::string resource_path = GetString(connection, "resourcePath");
    const std::string host = ParseEndpoint(resource_path).host;
    return host;
}

int GetConnectionPort(json_t* connection)
{
    const int direct_port = WireInteger(json_object_get(connection, "port"));
    if (direct_port > 0 && direct_port <= 65535)
        return direct_port;
    if (json_object_get(connection, "port"))
        return 0;
    const std::string resource = GetString(connection, "resourcePath");
    const size_t scheme = resource.find("://");
    if (scheme == std::string::npos)
        return 0;
    const std::string authority = resource.substr(scheme + 3, resource.find_first_of("/?", scheme + 3) - scheme - 3);
    if (authority.find(':', authority.empty() || authority.front() != '[' ? 0 : authority.find(']')) == std::string::npos)
        return 0;
    return ParseEndpoint(resource).port;
}

std::string BuildSignalingUrlFromConnection(json_t* connection)
{
    if (IsNativeConnection(connection) || WireInteger(json_object_get(connection, "usage")) != 14)
        return {};
    const std::string resource_path = GetString(connection, "resourcePath");
    if (resource_path.find("://") != std::string::npos)
        return ValidSignalingUrl(resource_path);
    if (!resource_path.empty() && resource_path.front() != '/')
        return {};
    const std::string ip = GetConnectionIp(connection);
    if (ip.empty())
        return {};
    const std::string host = ip.find(':') == std::string::npos ? ip : "[" + ip + "]";
    return ValidSignalingUrl("wss://" + host + ":443" + (resource_path.empty() ? "/nvst/" : resource_path));
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

} // namespace

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

    const std::string signaling_url = ValidSignalingUrl(GetString(sess, "signalingUrl"));
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

                if (WireInteger(json_object_get(conn_info, "usage")) != priority || IsNativeConnection(conn_info))
                    continue;
                if (priority == 14 && BuildSignalingUrlFromConnection(conn_info).empty())
                    continue;

                const std::string ip = GetConnectionIp(conn_info);
                const int port = GetConnectionPort(conn_info);
                if (!ip.empty() && port > 0 && (best_ip.empty() || (priority == 14 && port > best_port))) {
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
            for (bool explicit_url : {true, false}) {
                size_t index;
                json_t* conn_info;
                json_array_foreach(connection_info_array, index, conn_info) {
                    CollectIceServersFromObject(conn_info, info);

                    if (WireInteger(json_object_get(conn_info, "usage")) != 14)
                        continue;
                    const std::string resource = GetString(conn_info, "resourcePath");
                    if ((HasPrefix(resource, "wss://") || HasPrefix(resource, "https://")) != explicit_url)
                        continue;
                    info.signaling_url = BuildSignalingUrlFromConnection(conn_info);
                    if (!info.signaling_url.empty())
                        break;
                }
                if (!info.signaling_url.empty())
                    break;
            }
        }
    }

    info.server_ip = ParseEndpoint(info.signaling_url).host;
}

int WireInteger(json_t* value, int fallback)
{
    if (json_is_integer(value)) {
        const json_int_t number = json_integer_value(value);
        return number >= INT32_MIN && number <= INT32_MAX ? static_cast<int>(number) : fallback;
    }
    if (!json_is_string(value))
        return fallback;
    const std::string text = JsonString(value);
    int number = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() ? number : fallback;
}

std::string ResolveSessionControlBaseUrl(json_t* session, const std::string& fallback)
{
    json_t* control = json_object_get(session, "sessionControlInfo");
    const std::string host = Lowercase(GetString(control, "ip"));
    json_t* port = json_object_get(control, "port");
    if (!ValidHost(host) || (port && WireInteger(port) != 443))
        return fallback;
    const Endpoint origin = ParseEndpoint(fallback);
    if (host == Lowercase(origin.host))
        return "https://" + (host.find(':') == std::string::npos ? host : "[" + host + "]") + "/";
    const auto nvidia_host = [](const std::string& value) {
        return value.ends_with(".cloudmatchbeta.nvidiagrid.net") ||
            value.ends_with(".cloudmatch.nvidiagrid.net") ||
            value.ends_with(".geforcenow.nvidiagrid.net");
    };
    if (nvidia_host(Lowercase(origin.host)) && nvidia_host(host) && HasPrefix(host, "np-"))
        return "https://" + host + "/";
    return fallback;
}

bool CloudMatchResponseSucceeded(const HttpResponse& response)
{
    if (response.status_code < 200 || response.status_code >= 300)
        return false;
    if (response.body.empty())
        return true;
    try {
        JsonPtr root = LoadJson(response.body);
        return json_is_object(root.get()) && WireInteger(json_object_get(
            json_object_get(root.get(), "requestStatus"), "statusCode"), -1) == 1;
    } catch (...) {
        return false;
    }
}

SessionInfo ParseSessionResponse(const HttpResponse& response, const std::string& stage,
                                const std::string& expected_id, const std::string& base_url)
{
    JsonPtr root(nullptr, &json_decref);
    try { root = LoadJson(response.body); } catch (...) {}
    const bool patching = IsAppPatchingResponse(root.get());
    json_t* request = json_object_get(root.get(), "requestStatus");
    if (!json_is_object(root.get()) ||
        (!patching && (response.status_code < 200 || response.status_code >= 300 ||
            WireInteger(json_object_get(request, "statusCode"), -1) != 1)))
        throw std::runtime_error(BuildCloudMatchErrorMessage(stage, response.status_code, root.get()));
    json_t* sess = json_object_get(root.get(), "session");
    SessionInfo info;
    info.session_id = GetString(sess, "sessionId");
    if (info.session_id.empty() || info.session_id.find_first_of("/?#\\\r\n") != std::string::npos ||
        (!expected_id.empty() && expected_id != info.session_id))
        throw std::runtime_error(stage + " returned an invalid or different session ID.");
    info.status = ParseSessionStatus(json_object_get(sess, "status"));
    if (patching && !json_object_get(sess, "status"))
        info.status = 1;
    if (info.status < 0 || info.status > 8)
        throw std::runtime_error(stage + " returned an invalid or missing session status.");
    info.app_patching = patching;
    info.queue_position = WireInteger(json_object_get(sess, "queuePosition"), 0);
    ApplySeatSetupInfo(sess, info);
    info.queue_position = WireInteger(json_object_get(root.get(), "queuePosition"), info.queue_position);
    CollectIceServersFromObject(root.get(), info);
    ApplySessionNetworkInfo(sess, info);
    info.streaming_base_url = ResolveSessionControlBaseUrl(sess, base_url);
    const auto required = [](json_t* value) { return json_is_true(value) || WireInteger(value, 0) == 1; };
    info.ads_pending = required(json_object_get(sess, "sessionAdsRequired")) ||
        required(json_object_get(sess, "isAdsRequired")) ||
        required(json_object_get(json_object_get(sess, "sessionProgress"), "isAdsRequired"));
    json_t* ads = json_object_get(sess, "sessionAds");
    size_t index;
    json_t* ad;
    json_array_foreach(ads, index, ad) {
        std::string id = GetString(ad, "adId");
        if (json_is_integer(json_object_get(ad, "adId")))
            id = std::to_string(json_integer_value(json_object_get(ad, "adId")));
        if (!id.empty() && id.size() <= 256 &&
            std::find(info.ad_ids.begin(), info.ad_ids.end(), id) == info.ad_ids.end()) {
            if (info.ad_ids.size() >= 32)
                throw std::runtime_error("NVIDIA returned too many queue ads.");
            info.ad_ids.push_back(std::move(id));
        }
    }
    info.ads_pending = info.ads_pending || !info.ad_ids.empty();
    return info;
}

std::string BuildQueueAdCancellationBody(const std::string& id)
{
    JsonPtr root(json_object(), &json_decref);
    json_object_set_new(root.get(), "action", json_integer(6));
    json_t* updates = json_array();
    json_t* ad = json_object();
    json_object_set_new(ad, "adId", json_string(id.c_str()));
    json_object_set_new(ad, "adAction", json_integer(5));
    json_object_set_new(ad, "clientTimestamp", json_integer(std::time(nullptr)));
    json_object_set_new(ad, "cancelReason", json_string("FAILED_TO_REQUEST_ADS"));
    json_object_set_new(ad, "watchedTimeInMs", json_integer(0));
    json_object_set_new(ad, "pausedTimeInMs", json_integer(0));
    json_object_set_new(ad, "adDurationInMs", json_integer(0));
    json_array_append_new(updates, ad);
    json_object_set_new(root.get(), "adUpdates", updates);
    return DumpJson(root.get());
}

std::string BuildSessionBody(
    const std::string& app_id,
    const std::string& internal_title,
    const std::string& device_id,
    const std::string& sub_session_id,
    const std::string& network_test_session_id,
    const StreamSettings& stream_settings)
{
    JsonPtr root(json_object(), &json_decref);
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
    if (network_test_session_id.empty())
        json_object_set_new(req, "networkTestSessionId", json_null());
    else
        json_object_set_new(
            req, "networkTestSessionId",
            json_string(network_test_session_id.c_str()));
    json_object_set_new(req, "parentSessionId", json_null());
    json_object_set_new(req, "clientIdentification", json_string("GFN-PC"));
    json_object_set_new(req, "deviceHashId", json_string(device_id.c_str()));
    json_object_set_new(req, "clientVersion", json_string("30.0"));
    json_object_set_new(req, "clientPlatformName", json_string("browser"));

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
    json_object_set_new(
        req, "appLaunchMode",
        json_integer(ConsoleAppLaunchModeWireValue()));
    json_object_set_new(req, "secureRTSPSupported", json_false());
    json_object_set_new(req, "partnerCustomData", json_string(""));
    json_object_set_new(req, "accountLinked", json_true());
    json_object_set_new(
        req, "enablePersistingInGameSettings",
        json_boolean(stream_settings.persist_game_settings));
    json_object_set_new(req, "userAge", json_integer(26));
    const std::time_t now = std::time(nullptr);
    std::tm local_time {};
    std::tm utc_time {};
#if defined(_WIN32)
    localtime_s(&local_time, &now);
    gmtime_s(&utc_time, &now);
#else
    localtime_r(&now, &local_time);
    gmtime_r(&now, &utc_time);
#endif
    utc_time.tm_isdst = local_time.tm_isdst;
    const std::time_t local_epoch = std::mktime(&local_time);
    const std::time_t utc_as_local_epoch = std::mktime(&utc_time);
    const auto timezone_offset_ms = static_cast<json_int_t>(
        std::difftime(local_epoch, utc_as_local_epoch) * 1000.0);
    json_object_set_new(
        req, "clientTimezoneOffset", json_integer(timezone_offset_ms));

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
    json_object_set_new(features, "maxBitrateKbps", json_integer(stream_settings.bitrate_kbps));
    json_object_set_new(features, "codec", json_integer(1));
    json_object_set_new(features, "vsync", json_false());
    json_object_set_new(features, "audioChannelCount", json_integer(2));
    json_object_set_new(req, "requestedStreamingFeatures", features);

    json_t* meta = json_array();
    json_t* m1 = json_object(); json_object_set_new(m1, "key", json_string("SubSessionId")); json_object_set_new(m1, "value", json_string(sub_session_id.c_str())); json_array_append_new(meta, m1);
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

    json_object_set_new(root.get(), "sessionRequestData", req);
    return DumpJson(root.get());
}

std::string CreateNetworkTestSession(
    const HttpClient& http_client,
    const std::string& streaming_base_url,
    const std::vector<std::string>& headers,
    const StreamSettings& stream_settings)
{
    JsonPtr root(json_object(), &json_decref);
    json_t* request = json_object();
    json_t* profile = json_object();
    json_object_set_new(profile, "widthInPixels", json_integer(stream_settings.width));
    json_object_set_new(profile, "heightInPixels", json_integer(stream_settings.height));
    json_object_set_new(profile, "framesPerSecond", json_integer(stream_settings.fps));
    json_object_set_new(
        request, "clientPlatformName", json_string("browser"));
    json_object_set_new(request, "netTestProfile", profile);
    json_object_set_new(root.get(), "netTestRequestData", request);

    const HttpResponse response = http_client.Post(
        streaming_base_url + "v2/nettestsession",
        kCloudMatchUserAgent,
        headers,
        DumpJson(root.get()));
    if (response.status_code < 200 || response.status_code >= 300)
        return {};

    JsonPtr response_root = LoadJson(response.body);
    json_t* request_status = json_object_get(response_root.get(), "requestStatus");
    if (!request_status || GetInteger(request_status, "statusCode") != 1)
        return {};

    json_t* net_test_session =
        json_object_get(response_root.get(), "netTestSession");
    return GetString(net_test_session, "sessionId");
}

int ParseSessionStatus(json_t* status)
{
    const int numeric = WireInteger(status, -1);
    if (numeric >= 0)
        return numeric;

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


} // namespace gfn::cloud_session
} // namespace opennow
