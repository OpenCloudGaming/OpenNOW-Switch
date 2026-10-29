#include "cloud_session_internal.hpp"

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
                                    const StreamSettings& stream_settings,
                                    const std::string& launch_store,
                                    const std::string& internal_title) const
{
    session = EnsureFreshSavedSession(session);
    std::string jwt_token = ResolveSessionJwt(session);
    const std::string device_id = GenerateDeviceId();
    const std::string sub_session_id = GenerateUuid();

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
        "nv-client-streamer: WEBRTC",
        "nv-client-type: BROWSER",
        "nv-client-version: " + std::string(kCloudMatchClientVersion),
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

    HttpResponse response = http_client_.Post(url, kCloudMatchUserAgent, headers, body);
    if (response.status_code == 401)
    {
        session = ForceRefreshSavedSession(session);
        jwt_token = ResolveSessionJwt(session);
        headers[0] = "Authorization: GFNJWT " + jwt_token;
        AppendSessionTraceLog("START authorization rejected; refreshed token and retrying once");
        response = http_client_.Post(url, kCloudMatchUserAgent, headers, body);
    }
    AppendSessionTraceLog("START response HTTP " + std::to_string(response.status_code));
    AppendSessionTraceLog("START response body:\n" + JsonForTrace(response.body));

    SessionInfo info = ParseSessionResponse(response, "StartSession", {}, streaming_base_url);
    RememberActiveCloudSession(
        session, info.session_id, launch_app_id, info.streaming_base_url);
    ad_session_key_.clear();
    try {
        AnswerQueueAds(session, info, headers);
    } catch (...) {
        try { StopSession(session, info.session_id); } catch (...) {}
        throw;
    }
    AppendSessionTraceLog("START parsed: " + SessionInfoTraceSummary(info));
    return info;
}

SessionInfo GfnClient::PollSession(AuthSession& session, const std::string& session_id) const
{
    session = EnsureFreshSavedSession(session);
    std::string jwt_token = ResolveSessionJwt(session);
    const std::string device_id = GenerateDeviceId();
    std::string streaming_base_url =
        LoadActiveCloudSessionStreamingBaseUrl(session, session_id);
    if (streaming_base_url.empty())
        streaming_base_url = ResolveConfiguredStreamingBaseUrl(session);
    std::string url = streaming_base_url + "v2/session/" + session_id;

    std::vector<std::string> headers = {
        "Authorization: GFNJWT " + jwt_token,
        "nv-client-id: " + client_id_,
        "nv-browser-type: CHROME",
        "nv-client-streamer: WEBRTC",
        "nv-client-type: BROWSER",
        "nv-client-version: " + std::string(kCloudMatchClientVersion),
        "nv-device-make: UNKNOWN",
        "nv-device-model: UNKNOWN",
        "nv-device-os: WINDOWS",
        "nv-device-type: DESKTOP",
        "x-device-id: " + device_id,
        "Content-Type: application/json"
    };

    AppendSessionTraceLog("POLL url=" + url);
    AppendSessionTraceLog("POLL headers:\n" + HeadersForTrace(headers));
    HttpResponse response = http_client_.Get(url, kCloudMatchUserAgent, headers);
    if (response.status_code == 401)
    {
        session = ForceRefreshSavedSession(session);
        jwt_token = ResolveSessionJwt(session);
        headers[0] = "Authorization: GFNJWT " + jwt_token;
        AppendSessionTraceLog("POLL authorization rejected; refreshed token and retrying once");
        response = http_client_.Get(url, kCloudMatchUserAgent, headers);
    }
    AppendSessionTraceLog("POLL response HTTP " + std::to_string(response.status_code));
    AppendSessionTraceLog("POLL response body:\n" + JsonForTrace(response.body));

    SessionInfo info = ParseSessionResponse(response, "PollSession", session_id, streaming_base_url);
    UpdateActiveCloudSessionStreamingBaseUrl(session, session_id, info.streaming_base_url);
    AnswerQueueAds(session, info, headers);
    AppendSessionTraceLog("POLL parsed: " + SessionInfoTraceSummary(info));
    return info;
}

void GfnClient::AnswerQueueAds(AuthSession& auth, SessionInfo& info,
                              std::vector<std::string> headers) const
{
    if (info.status > 3 && info.status != 6)
        return;
    const std::string key = auth.user.user_id + ":" + info.session_id;
    if (ad_session_key_ != key) {
        ad_session_key_ = key;
        ad_attempts_.clear();
        ad_pending_polls_ = 0;
    }
    for (const std::string& id : info.ad_ids) {
        if (ad_attempts_.size() >= 32 && !ad_attempts_.contains(id))
            throw std::runtime_error("NVIDIA returned too many queue ads.");
        ad_attempts_.try_emplace(id, 0);
    }
    bool pending = info.ads_pending && ad_attempts_.empty();
    for (auto& [id, attempts] : ad_attempts_) {
        if (attempts < 0)
            continue;
        if (attempts >= 3)
            throw std::runtime_error("Unable to report unsupported NVIDIA queue ad playback. Please try again.");
        ++attempts;
        HttpResponse response;
        const std::string body = BuildQueueAdCancellationBody(id);
        const std::string url = info.streaming_base_url + "v2/session/" + info.session_id;
        try {
            response = http_client_.Request("PUT", url, kCloudMatchUserAgent, headers, body);
        } catch (const std::exception&) {
            pending = true;
            break;
        }
        if (response.status_code == 401) {
            auth = ForceRefreshSavedSession(auth);
            headers[0] = "Authorization: GFNJWT " + ResolveSessionJwt(auth);
            try {
                response = http_client_.Request("PUT", url, kCloudMatchUserAgent, headers, body);
            } catch (const std::exception&) {
                pending = true;
                break;
            }
        }
        if (CloudMatchResponseSucceeded(response)) {
            attempts = -1;
            AppendSessionTraceLog("QUEUEAD reported unsupported playback");
            continue;
        }
        if (response.status_code >= 400 && response.status_code < 500 && response.status_code != 429)
            throw std::runtime_error("NVIDIA rejected the queue ad cancellation (HTTP " +
                                     std::to_string(response.status_code) + ").");
        pending = true;
        break;
    }
    info.ads_pending = pending || std::any_of(ad_attempts_.begin(), ad_attempts_.end(),
        [](const auto& entry) { return entry.second >= 0; });
    if (std::any_of(ad_attempts_.begin(), ad_attempts_.end(),
        [](const auto& entry) { return entry.second >= 3; }))
        throw std::runtime_error("Unable to report unsupported NVIDIA queue ad playback. Please try again.");
    if (info.ads_pending) {
        if (++ad_pending_polls_ >= 12)
            throw std::runtime_error("NVIDIA queue ads did not become available for cancellation in time.");
    } else {
        ad_pending_polls_ = 0;
    }
}

void GfnClient::StopSession(AuthSession& session, const std::string& session_id) const
{
    session = EnsureFreshSavedSession(session);
    std::string jwt_token = ResolveSessionJwt(session);
    const std::string device_id = GenerateDeviceId();
    std::string streaming_base_url =
        LoadActiveCloudSessionStreamingBaseUrl(session, session_id);
    if (streaming_base_url.empty())
        streaming_base_url = ResolveConfiguredStreamingBaseUrl(session);
    std::string url = streaming_base_url + "v2/session/" + session_id;

    std::vector<std::string> headers = {
        "Authorization: GFNJWT " + jwt_token,
        "nv-client-id: " + client_id_,
        "nv-browser-type: CHROME",
        "nv-client-streamer: WEBRTC",
        "nv-client-type: BROWSER",
        "nv-client-version: " + std::string(kCloudMatchClientVersion),
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
        "DELETE", url, kCloudMatchUserAgent, headers);
    if (response.status_code == 401)
    {
        session = ForceRefreshSavedSession(session);
        jwt_token = ResolveSessionJwt(session);
        headers[0] = "Authorization: GFNJWT " + jwt_token;
        AppendSessionTraceLog("STOP authorization rejected; refreshed token and retrying once");
        response = http_client_.Request(
            "DELETE", url, kCloudMatchUserAgent, headers);
    }
    AppendSessionTraceLog("STOP response HTTP " + std::to_string(response.status_code));
    AppendSessionTraceLog("STOP response body:\n" + JsonForTrace(response.body));
    if (CloudMatchResponseSucceeded(response) || response.status_code == 404)
    {
        ForgetActiveCloudSession(session_id);
        if (ad_session_key_ == session.user.user_id + ":" + session_id) {
            ad_session_key_.clear();
            ad_attempts_.clear();
            ad_pending_polls_ = 0;
        }
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
