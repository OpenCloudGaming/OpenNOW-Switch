#include "gfn/cloud_session_internal.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace
{
bool reject_next_request = false;
int request_count = 0;
int refresh_count = 0;
opennow::StreamSettings settings;
}

namespace opennow
{
bool StreamDiagnosticsEnabled() { return false; }

StreamSettings LoadStreamSettings() { return settings; }

AuthSession GfnClient::EnsureFreshSavedSession(const AuthSession& session) const
{
    return session;
}

AuthSession GfnClient::ForceRefreshSavedSession(const AuthSession& session) const
{
    ++refresh_count;
    auto refreshed = session;
    refreshed.tokens.id_token = "synthetic-refreshed-" + std::to_string(refresh_count);
    return refreshed;
}

std::vector<StreamRegion> GfnClient::FetchStreamRegions(AuthSession&) const
{
    return {};
}

std::vector<StreamRegion> GfnClient::MeasureStreamRegionLatencies(
    std::vector<StreamRegion> regions) const
{
    return regions;
}

HttpResponse HttpClient::Request(
    const std::string& method, const std::string& url,
    const std::string& user_agent, const std::vector<std::string>& headers,
    const std::string& body, const std::string&, HttpTransferControl) const
{
    ++request_count;
    assert(std::find(headers.begin(), headers.end(), "nv-client-type: BROWSER") != headers.end());
    assert(std::find(headers.begin(), headers.end(), "nv-client-streamer: WEBRTC") != headers.end());
    assert(std::find(headers.begin(), headers.end(), "nv-client-version: 2.0.89.154") != headers.end());
    assert(user_agent.find("Chrome/") != std::string::npos);
    assert(user_agent.find("NVIDIACEFClient") == std::string::npos);
    assert(user_agent.find("GFN-PC/") == std::string::npos);
    const std::string token = refresh_count == 0
        ? "synthetic-initial"
        : "synthetic-refreshed-" + std::to_string(refresh_count);
    assert(std::find(headers.begin(), headers.end(), "Authorization: GFNJWT " + token) != headers.end());

    const bool network_test = url.ends_with("v2/nettestsession");
    if (method == "POST")
    {
        const auto root = gfn::detail::LoadJson(body);
        json_t* request = json_object_get(
            root.get(), network_test ? "netTestRequestData" : "sessionRequestData");
        assert(gfn::detail::GetString(request, "clientPlatformName") == "browser");
        if (!network_test)
        {
            assert(gfn::detail::GetInteger(request, "appLaunchMode") == 2);
            assert(gfn::detail::GetInteger(request, "remoteControllersBitmap") == 1);
            assert(gfn::detail::GetString(request, "networkTestSessionId") == "synthetic-nettest");
        }
    }
    if (network_test)
        return {200, R"({"requestStatus":{"statusCode":1},"netTestSession":{"sessionId":"synthetic-nettest"}})"};
    if (reject_next_request)
    {
        reject_next_request = false;
        return {401, "{}"};
    }
    return {200, R"({"requestStatus":{"statusCode":1},"session":{"sessionId":"synthetic-session","status":1}})"};
}

HttpResponse HttpClient::Get(
    const std::string& url, const std::string& user_agent,
    const std::vector<std::string>& headers, const std::string& proxy,
    HttpTransferControl control) const
{
    return Request("GET", url, user_agent, headers, {}, proxy, control);
}

HttpResponse HttpClient::Post(
    const std::string& url, const std::string& user_agent,
    const std::vector<std::string>& headers, const std::string& body,
    const std::string& proxy, HttpTransferControl control) const
{
    return Request("POST", url, user_agent, headers, body, proxy, control);
}
}

int main()
{
    const auto original_directory = std::filesystem::current_path();
    std::string pattern = (std::filesystem::temp_directory_path() / "opennow-cloudmatch-XXXXXX").string();
    const char* directory = mkdtemp(pattern.data());
    assert(directory);
    std::filesystem::current_path(directory);
    std::filesystem::create_directories(opennow::gfn::detail::GetAppHome());
    settings.region = "https://cloudmatch.example/";

    for (const bool retry : {false, true})
    {
        request_count = 0;
        refresh_count = 0;
        opennow::AuthSession session;
        session.user.user_id = "synthetic-user";
        session.tokens.id_token = "synthetic-initial";
        session.provider.streaming_service_url = settings.region;
        opennow::GfnClient client;

        reject_next_request = retry;
        const auto started = client.StartSession(session, "123", settings);
        assert(started.session_id == "synthetic-session");
        reject_next_request = retry;
        const auto polled = client.PollSession(session, started.session_id);
        assert(polled.session_id == started.session_id);
        reject_next_request = retry;
        client.StopSession(session, started.session_id);
        assert(request_count == (retry ? 7 : 4));
        assert(refresh_count == (retry ? 3 : 0));
        assert(opennow::gfn::cloud_session::LoadActiveCloudSessionId(session).empty());
    }

    std::filesystem::current_path(original_directory);
    std::filesystem::remove_all(directory);
}
