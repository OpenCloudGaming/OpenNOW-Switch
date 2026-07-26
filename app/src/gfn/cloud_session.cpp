#include "cloud_session_internal.hpp"

#include "../community_proxy_policy.hpp"
#include "../server_location_policy.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace opennow
{
using namespace gfn::detail;
using namespace gfn::cloud_session;

SessionInfo GfnClient::StartSession(AuthSession& session, const std::string& launch_app_id,
                                    const std::string& launch_store,
                                    const std::string& internal_title) const
{
    session = EnsureFreshSavedSession(session);
    std::string jwt_token = ResolveSessionJwt(session);
    const std::string device_id = GenerateDeviceId();
    const std::string sub_session_id = GenerateUuid();

    const StreamSettings stream_settings = LoadStreamSettings();
    const std::string proxy_url = community_proxy::EnabledUrl(stream_settings);
    std::string automatic_region_url;
    std::string automatic_region_trace;
    if (server_location::IsAutomatic(stream_settings.region))
    {
        try
        {
            std::vector<StreamRegion> regions = FetchStreamRegions(session);
            regions = MeasureStreamRegionLatencies(std::move(regions));
            automatic_region_url =
                server_location::SelectBestMeasuredRegionUrl(regions);

            const auto selected = std::find_if(
                regions.begin(), regions.end(),
                [&automatic_region_url](const StreamRegion& region) {
                    return server_location::NormalizeStreamingBaseUrl(region.url) ==
                        automatic_region_url;
                });
            if (selected != regions.end())
            {
                automatic_region_trace =
                    "Auto selected=" + selected->name +
                    " ping=" + std::to_string(selected->ping_ms) + "ms";
            }
            else
            {
                automatic_region_trace =
                    "Auto found no reachable regional endpoint; using provider fallback";
            }
        }
        catch (const std::exception& ex)
        {
            automatic_region_trace =
                "Auto region test failed; using provider fallback: " +
                std::string(ex.what());
        }
    }
    const std::string streaming_base_url = server_location::ResolveStreamingBaseUrl(
        stream_settings.region,
        session.provider.streaming_service_url,
        automatic_region_url);
    if (streaming_base_url.empty())
        throw std::runtime_error("The selected GeForce NOW server location is invalid.");
    std::string url = streaming_base_url +
        "v2/session?keyboardLayout=en-US_qwerty&languageCode=" +
        stream_settings.game_language;

    std::vector<std::string> headers = {
        "Authorization: GFNJWT " + jwt_token,
        "Content-Type: application/json",
        "nv-client-id: " + client_id_,
        "nv-browser-type: CHROME",
        "nv-client-streamer: NVIDIA-CLASSIC",
        "nv-client-type: NATIVE",
        "nv-client-version: 2.0.80.173",
        "nv-device-make: UNKNOWN",
        "nv-device-model: UNKNOWN",
        "nv-device-os: WINDOWS",
        "nv-device-type: DESKTOP",
        "x-device-id: " + device_id,
        "Origin: https://play.geforcenow.com",
        "Referer: https://play.geforcenow.com/"
    };

    std::string network_test_session_id;
    std::string network_test_trace;
    try
    {
        network_test_session_id = CreateNetworkTestSession(
            http_client_,
            streaming_base_url,
            headers,
            proxy_url,
            stream_settings);
        network_test_trace = network_test_session_id.empty()
            ? "unavailable; launch will use regional endpoint without a test ID"
            : "created";
    }
    catch (const std::exception& ex)
    {
        network_test_trace =
            "failed; launch will use regional endpoint without a test ID: " +
            std::string(ex.what());
    }
    std::string body = BuildSessionBody(
        launch_app_id,
        internal_title,
        device_id,
        sub_session_id,
        network_test_session_id,
        stream_settings);
    ResetSessionTraceLog("StartSession appId=" + launch_app_id +
                         " store=" + (launch_store.empty() ? "unknown" : launch_store) +
                         " internalTitle=" + (internal_title.empty() ? "missing" : internal_title));
    if (!automatic_region_trace.empty())
        AppendSessionTraceLog("START region=" + automatic_region_trace);
    AppendSessionTraceLog("START nettest=" + network_test_trace);
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

    HttpResponse response = http_client_.Post(url, kUserAgent, headers, body, proxy_url);
    if (response.status_code == 401)
    {
        session = ForceRefreshSavedSession(session);
        jwt_token = ResolveSessionJwt(session);
        headers[0] = "Authorization: GFNJWT " + jwt_token;
        AppendSessionTraceLog("START authorization rejected; refreshed token and retrying once");
        response = http_client_.Post(url, kUserAgent, headers, body, proxy_url);
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
    RememberActiveCloudSession(
        session, info.session_id, launch_app_id, streaming_base_url);
    AppendSessionTraceLog("START parsed: " + SessionInfoTraceSummary(info));
    return info;
}

SessionInfo GfnClient::PollSession(AuthSession& session, const std::string& session_id) const
{
    session = EnsureFreshSavedSession(session);
    std::string jwt_token = ResolveSessionJwt(session);
    const std::string device_id = GenerateDeviceId();
    const std::string proxy_url = community_proxy::EnabledUrl(LoadStreamSettings());
    std::string streaming_base_url =
        LoadActiveCloudSessionStreamingBaseUrl(session, session_id);
    if (streaming_base_url.empty())
        streaming_base_url = ResolveConfiguredStreamingBaseUrl(session);
    std::string url = streaming_base_url + "v2/session/" + session_id;

    std::vector<std::string> headers = {
        "Authorization: GFNJWT " + jwt_token,
        "nv-client-id: " + client_id_,
        "nv-browser-type: CHROME",
        "nv-client-streamer: NVIDIA-CLASSIC",
        "nv-client-type: NATIVE",
        "nv-client-version: 2.0.80.173",
        "nv-device-make: UNKNOWN",
        "nv-device-model: UNKNOWN",
        "nv-device-os: WINDOWS",
        "nv-device-type: DESKTOP",
        "x-device-id: " + device_id,
        "Content-Type: application/json"
    };

    AppendSessionTraceLog("POLL url=" + url);
    AppendSessionTraceLog("POLL headers:\n" + HeadersForTrace(headers));
    HttpResponse response = http_client_.Get(url, kUserAgent, headers, proxy_url);
    if (response.status_code == 401)
    {
        session = ForceRefreshSavedSession(session);
        jwt_token = ResolveSessionJwt(session);
        headers[0] = "Authorization: GFNJWT " + jwt_token;
        AppendSessionTraceLog("POLL authorization rejected; refreshed token and retrying once");
        response = http_client_.Get(url, kUserAgent, headers, proxy_url);
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
    const std::string device_id = GenerateDeviceId();
    const std::string proxy_url = community_proxy::EnabledUrl(LoadStreamSettings());
    std::string streaming_base_url =
        LoadActiveCloudSessionStreamingBaseUrl(session, session_id);
    if (streaming_base_url.empty())
        streaming_base_url = ResolveConfiguredStreamingBaseUrl(session);
    std::string url = streaming_base_url + "v2/session/" + session_id;

    std::vector<std::string> headers = {
        "Authorization: GFNJWT " + jwt_token,
        "nv-client-id: " + client_id_,
        "nv-browser-type: CHROME",
        "nv-client-streamer: NVIDIA-CLASSIC",
        "nv-client-type: NATIVE",
        "nv-client-version: 2.0.80.173",
        "nv-device-make: UNKNOWN",
        "nv-device-model: UNKNOWN",
        "nv-device-os: WINDOWS",
        "nv-device-type: DESKTOP",
        "x-device-id: " + device_id,
        "Content-Type: application/json"
    };

    AppendSessionTraceLog("STOP url=" + url);
    AppendSessionTraceLog("STOP headers:\n" + HeadersForTrace(headers));
    HttpResponse response = http_client_.Request(
        "DELETE", url, kUserAgent, headers, {}, proxy_url);
    if (response.status_code == 401)
    {
        session = ForceRefreshSavedSession(session);
        jwt_token = ResolveSessionJwt(session);
        headers[0] = "Authorization: GFNJWT " + jwt_token;
        AppendSessionTraceLog("STOP authorization rejected; refreshed token and retrying once");
        response = http_client_.Request(
            "DELETE", url, kUserAgent, headers, {}, proxy_url);
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
    try
    {
        StopSession(session, stale_session_id);
    }
    catch (const std::exception& e)
    {
        const std::string error = e.what();
        if (error.find("HTTP 403") == std::string::npos)
            throw;

        // Builds before the stable-device fix created CloudMatch sessions with
        // a shared placeholder identity. They cannot be deleted with the new,
        // installation-specific identity and must not block the next launch.
        AppendAuthLog("session: discarded legacy stale-session marker after HTTP 403");
        ForgetActiveCloudSession(stale_session_id);
    }
}


} // namespace opennow
