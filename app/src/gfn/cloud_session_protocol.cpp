#include "cloud_session_internal.hpp"
#include "app_launch_mode_policy.hpp"

#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <utility>

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
    json_object_set_new(
        req, "appLaunchMode",
        json_integer(AppLaunchModeWireValue(stream_settings.launch_in_console_mode)));
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
    const std::string& proxy_url,
    const StreamSettings& stream_settings)
{
    JsonPtr root(json_object(), &json_decref);
    json_t* request = json_object();
    json_t* profile = json_object();
    json_object_set_new(profile, "widthInPixels", json_integer(stream_settings.width));
    json_object_set_new(profile, "heightInPixels", json_integer(stream_settings.height));
    json_object_set_new(profile, "framesPerSecond", json_integer(stream_settings.fps));
    json_object_set_new(
        request, "clientPlatformName", json_string("windows"));
    json_object_set_new(request, "netTestProfile", profile);
    json_object_set_new(root.get(), "netTestRequestData", request);

    const HttpResponse response = http_client.Post(
        streaming_base_url + "v2/nettestsession",
        GfnClient::kUserAgent,
        headers,
        DumpJson(root.get()),
        proxy_url);
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


} // namespace gfn::cloud_session
} // namespace opennow
