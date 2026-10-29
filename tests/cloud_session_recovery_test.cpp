#include "gfn/cloud_session_internal.hpp"

#include <cassert>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>

namespace
{
struct Reply
{
    std::string method;
    std::string url;
    opennow::HttpResponse response;
    std::function<void(const std::string&)> inspect;
};
std::deque<Reply> replies;
opennow::StreamSettings settings;
const std::string initial = "https://np-ams-06.cloudmatchbeta.nvidiagrid.net/";
const std::string assigned = "https://np-par-01.cloudmatchbeta.nvidiagrid.net/";
const std::string queued = R"({"requestStatus":{"statusCode":1},"session":{"sessionId":"seat","status":1,"sessionControlInfo":{"ip":"np-par-01.cloudmatchbeta.nvidiagrid.net","port":443}}})";
const std::string with_ads = R"({"requestStatus":{"statusCode":1},"session":{"sessionId":"seat","status":1,"sessionAdsRequired":1,"sessionAds":[{"adId":"ad-one"},{"adId":"ad-one"}],"sessionControlInfo":{"ip":"np-par-01.cloudmatchbeta.nvidiagrid.net","port":443}}})";

void expect(const std::string& method, const std::string& url, int code, const std::string& body,
            std::function<void(const std::string&)> inspect = {})
{
    replies.push_back({method, url, {code, body}, std::move(inspect)});
}

void expect_start(const std::string& body)
{
    expect("POST", initial + "v2/nettestsession", 200,
           R"({"requestStatus":{"statusCode":1},"netTestSession":{"sessionId":"nettest"}})");
    expect("POST", initial + "v2/session?keyboardLayout=en-US_qwerty&languageCode=en_US", 200, body);
}

void inspect_ad(const std::string& body)
{
    const auto root = opennow::gfn::detail::LoadJson(body);
    assert(opennow::gfn::detail::GetInteger(root.get(), "action") == 6);
    json_t* updates = json_object_get(root.get(), "adUpdates");
    assert(json_array_size(updates) == 1);
    json_t* ad = json_array_get(updates, 0);
    assert(opennow::gfn::detail::GetString(ad, "adId") == "ad-one");
    assert(opennow::gfn::detail::GetInteger(ad, "adAction") == 5);
    assert(opennow::gfn::detail::GetString(ad, "cancelReason") == "FAILED_TO_REQUEST_ADS");
    for (const char* key : {"watchedTimeInMs", "pausedTimeInMs", "adDurationInMs"}) {
        assert(json_is_integer(json_object_get(ad, key)));
        assert(opennow::gfn::detail::GetInteger(ad, key) == 0);
    }
    assert(json_integer_value(json_object_get(ad, "clientTimestamp")) > 0);
}
}

namespace opennow
{
bool StreamDiagnosticsEnabled() { return false; }
StreamSettings LoadStreamSettings() { return settings; }
AuthSession GfnClient::EnsureFreshSavedSession(const AuthSession& auth) const { return auth; }
AuthSession GfnClient::ForceRefreshSavedSession(const AuthSession& auth) const { return auth; }
std::vector<StreamRegion> GfnClient::FetchStreamRegions(AuthSession&) const { return {}; }
std::vector<StreamRegion> GfnClient::MeasureStreamRegionLatencies(std::vector<StreamRegion> regions) const { return regions; }

HttpResponse HttpClient::Request(const std::string& method, const std::string& url,
    const std::string&, const std::vector<std::string>&, const std::string& body,
    const std::string&, HttpTransferControl) const
{
    assert(!replies.empty());
    auto reply = std::move(replies.front());
    replies.pop_front();
    assert(method == reply.method);
    assert(url == reply.url);
    if (reply.inspect)
        reply.inspect(body);
    return reply.response;
}
HttpResponse HttpClient::Get(const std::string& url, const std::string& ua,
    const std::vector<std::string>& headers, const std::string& proxy, HttpTransferControl control) const
{
    return Request("GET", url, ua, headers, {}, proxy, control);
}
HttpResponse HttpClient::Post(const std::string& url, const std::string& ua,
    const std::vector<std::string>& headers, const std::string& body,
    const std::string& proxy, HttpTransferControl control) const
{
    return Request("POST", url, ua, headers, body, proxy, control);
}
}

int main(int argc, char** argv)
{
    const int selected = argc > 1 ? std::atoi(argv[1]) : -1;
    const auto cwd = std::filesystem::current_path();
    std::string pattern = (std::filesystem::temp_directory_path() / "opennow-recovery-XXXXXX").string();
    const char* directory = mkdtemp(pattern.data());
    assert(directory);
    std::filesystem::current_path(directory);
    std::filesystem::create_directories(opennow::gfn::detail::GetAppHome());
    settings.region = initial;
    settings.game_language = "en_US";
    opennow::AuthSession auth;
    auth.user.user_id = "synthetic-user";
    auth.tokens.id_token = "synthetic-token";
    auth.provider.streaming_service_url = initial;
    opennow::GfnClient client;
    if (selected < 0 || selected == 0) {
        expect_start(queued);
        client.StartSession(auth, "123", settings);
        assert(opennow::gfn::cloud_session::LoadActiveCloudSessionStreamingBaseUrl(auth, "seat") == assigned);
        expect("GET", assigned + "v2/session/seat", 200, queued);
        client.PollSession(auth, "seat");
        expect("DELETE", assigned + "v2/session/seat", 204, "");
        client.StopSession(auth, "seat");
    }
    if (selected < 0 || selected == 1) {
        for (const char* body : {"{}", R"({"requestStatus":{"statusCode":1},"session":{"sessionId":"other","status":2}})",
                 R"({"requestStatus":{"statusCode":1},"session":{"sessionId":"seat"}})"}) {
            expect("GET", initial + "v2/session/seat", 200, body);
            bool threw = false;
            try { client.PollSession(auth, "seat"); } catch (const std::exception&) { threw = true; }
            assert(threw);
        }
    }
    if (selected < 0 || selected == 2) {
        opennow::gfn::cloud_session::RememberActiveCloudSession(auth, "seat", "123", initial);
        expect("DELETE", initial + "v2/session/seat", 200, R"({"requestStatus":{"statusCode":4}})");
        bool threw = false;
        try { client.StopSession(auth, "seat"); } catch (const std::exception&) { threw = true; }
        assert(threw);
        assert(opennow::gfn::cloud_session::LoadActiveCloudSessionId(auth) == "seat");
        opennow::gfn::cloud_session::ForgetActiveCloudSession("seat");
    }
    if (selected < 0 || selected == 3) {
        opennow::gfn::cloud_session::RememberActiveCloudSession(auth, "seat", "123", assigned);
        opennow::gfn::cloud_session::RememberActiveCloudSession(auth, "seat", "123", assigned);
        const std::string path = opennow::gfn::detail::GetAppHome() + "/active_cloud_session.json";
        std::filesystem::remove(path);
        assert(opennow::gfn::cloud_session::LoadActiveCloudSessionId(auth) == "seat");
        assert(opennow::gfn::cloud_session::LoadActiveCloudSessionStreamingBaseUrl(auth, "seat") == assigned);
        std::ofstream(path) << "{";
        assert(opennow::gfn::cloud_session::LoadActiveCloudSessionId(auth) == "seat");
        opennow::gfn::cloud_session::ForgetActiveCloudSession("seat");
    }
    if (selected < 0 || selected == 4) {
        expect_start(with_ads);
        expect("PUT", assigned + "v2/session/seat", 503, "{}", inspect_ad);
        client.StartSession(auth, "123", settings);
        expect("GET", assigned + "v2/session/seat", 200, queued);
        expect("PUT", assigned + "v2/session/seat", 200, R"({"requestStatus":{"statusCode":1}})", inspect_ad);
        client.PollSession(auth, "seat");
        expect("GET", assigned + "v2/session/seat", 200, with_ads);
        client.PollSession(auth, "seat");
        assert(replies.empty());
        opennow::gfn::cloud_session::ForgetActiveCloudSession("seat");
    }
    if (selected < 0 || selected == 5) {
        for (const char* host : {"evil.example", "np-par-01.cloudmatchbeta.nvidiagrid.net.evil.example",
                "np-par-01.cloudmatchbeta.nvidiagrid.net@evil.example", ".cloudmatchbeta.nvidiagrid.net",
                "np..cloudmatchbeta.nvidiagrid.net", "http://np-par-01.cloudmatchbeta.nvidiagrid.net",
                "np-par-01.cloudmatchbeta.nvidiagrid.net?x=1", "np-par-01.cloudmatchbeta.nvidiagrid.net#x"}) {
            auto root = opennow::gfn::detail::LoadJson(std::string("{\"sessionControlInfo\":{\"ip\":\"") + host + "\",\"port\":443}}");
            assert(opennow::gfn::cloud_session::ResolveSessionControlBaseUrl(root.get(), initial) == initial);
        }
        for (const char* port : {"80", "8443", "0", "null", "\"bogus\""}) {
            auto root = opennow::gfn::detail::LoadJson(std::string("{\"sessionControlInfo\":{\"ip\":\"np-par-01.cloudmatchbeta.nvidiagrid.net\",\"port\":") + port + "}}");
            assert(opennow::gfn::cloud_session::ResolveSessionControlBaseUrl(root.get(), initial) == initial);
        }
        auto root = opennow::gfn::detail::LoadJson(R"({"sessionControlInfo":{"ip":"seat.partner.example","port":443}})");
        assert(opennow::gfn::cloud_session::ResolveSessionControlBaseUrl(root.get(), "https://seat.partner.example/") == "https://seat.partner.example/");
        assert(opennow::gfn::cloud_session::ResolveSessionControlBaseUrl(root.get(), initial) == initial);
        auto nvidia = opennow::gfn::detail::LoadJson(R"({"sessionControlInfo":{"ip":"NP-PAR-01.cloudmatchbeta.nvidiagrid.net","port":"443"}})");
        assert(opennow::gfn::cloud_session::ResolveSessionControlBaseUrl(nvidia.get(), initial) == assigned);
    }
    if (selected < 0 || selected == 6) {
        const std::string ready = R"({"requestStatus":{"statusCode":"1"},"session":{"sessionId":"seat","status":"2","connectionInfo":[{"usage":"14","appLevelProtocol":"5","ip":"seat.example","port":"443","resourcePath":"/nvst/"}]}})";
        expect("GET", initial + "v2/session/seat", 200, ready);
        auto info = client.PollSession(auth, "seat");
        assert(info.ready_for_stream());
        expect("GET", initial + "v2/session/seat", 200, R"({"requestStatus":{"statusCode":1},"session":{"sessionId":"seat","status":2}})");
        info = client.PollSession(auth, "seat");
        assert(!info.ready_for_stream());
        assert(info.signaling_url.empty());
        assert(info.media_ip.empty());
        assert(info.ice_servers.empty());
        expect("GET", initial + "v2/session/seat", 200, ready);
        assert(client.PollSession(auth, "seat").ready_for_stream());
    }
    if (selected < 0 || selected == 7) {
        expect_start(with_ads);
        expect("PUT", assigned + "v2/session/seat", 200, R"({"requestStatus":{"statusCode":4}})", inspect_ad);
        auto info = client.StartSession(auth, "123", settings);
        assert(info.ads_pending);
        for (int attempt = 1; attempt < 3; ++attempt) {
            expect("GET", assigned + "v2/session/seat", 200, queued);
            expect("PUT", assigned + "v2/session/seat", 503, "{}", inspect_ad);
            bool threw = false;
            try { info = client.PollSession(auth, "seat"); } catch (const std::exception&) { threw = true; }
            assert(threw == (attempt == 2));
        }
        opennow::gfn::cloud_session::ForgetActiveCloudSession("seat");
    }
    if (selected < 0 || selected == 8) {
        for (const bool cleanup_succeeds : {false, true}) {
            expect_start(with_ads);
            expect("PUT", assigned + "v2/session/seat", 403, "{}", inspect_ad);
            expect("DELETE", assigned + "v2/session/seat", cleanup_succeeds ? 204 : 503, "");
            bool threw = false;
            try { client.StartSession(auth, "123", settings); } catch (const std::exception&) { threw = true; }
            assert(threw);
            assert(opennow::gfn::cloud_session::LoadActiveCloudSessionId(auth).empty() == cleanup_succeeds);
            opennow::gfn::cloud_session::ForgetActiveCloudSession("seat");
        }
    }
    if (selected < 0 || selected == 9) {
        opennow::gfn::cloud_session::RememberActiveCloudSession(auth, "seat", "123", initial);
        expect("GET", initial + "v2/session/seat", 200, queued);
        client.PollSession(auth, "seat");
        assert(opennow::gfn::cloud_session::LoadActiveCloudSessionStreamingBaseUrl(auth, "seat") == assigned);
        opennow::AuthSession other = auth;
        other.user.user_id = "different-user";
        assert(opennow::gfn::cloud_session::LoadActiveCloudSessionId(other).empty());
        assert(opennow::gfn::cloud_session::LoadActiveCloudSessionStreamingBaseUrl(other, "seat").empty());
        expect("DELETE", assigned + "v2/session/seat", 200, R"({"requestStatus":{"statusCode":"1"}})");
        client.StopSession(auth, "seat");
    }
    if (selected < 0 || selected == 10) {
        const std::string required = R"({"requestStatus":{"statusCode":1},"session":{"sessionId":"seat","status":1,"sessionAdsRequired":true}})";
        expect_start(required);
        assert(client.StartSession(auth, "123", settings).ads_pending);
        for (int poll = 1; poll < 12; ++poll) {
            expect("GET", initial + "v2/session/seat", 200, required);
            bool threw = false;
            try { client.PollSession(auth, "seat"); } catch (const std::exception&) { threw = true; }
            assert(threw == (poll == 11));
        }
        opennow::gfn::cloud_session::ForgetActiveCloudSession("seat");
    }
    assert(replies.empty());
    std::filesystem::current_path(cwd);
    std::filesystem::remove_all(directory);
}
