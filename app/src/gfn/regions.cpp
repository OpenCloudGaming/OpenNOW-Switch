#include "internal.hpp"

#include "../server_location_policy.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace opennow
{
using namespace gfn::detail;

namespace
{
constexpr const char* kLcarsClientId = "ec7e38d4-03af-4b58-b131-cfb0495903ab";
constexpr const char* kClientVersion = "2.0.80.173";

std::vector<std::string> BuildRegionHeaders(const std::string& token)
{
    std::vector<std::string> headers = {
        "Accept: application/json",
        "nv-client-id: " + std::string(kLcarsClientId),
        "nv-client-type: BROWSER",
        "nv-client-version: " + std::string(kClientVersion),
        "nv-client-streamer: WEBRTC",
        "nv-device-os: WINDOWS",
        "nv-device-type: DESKTOP",
        "User-Agent: " + std::string(GfnClient::kUserAgent),
    };
    if (!token.empty())
        headers.push_back("Authorization: GFNJWT " + token);
    return headers;
}

int MeasureAverageLatency(const HttpClient& http_client, const std::string& url)
{
    // Match the desktop app: one warm-up connect, then average three measured
    // TCP connects. The requests are direct so a selected HTTP proxy cannot
    // disguise the latency between the console and a streaming location.
    (void)http_client.MeasureConnectLatencyMs(url);

    int total_ms = 0;
    int successful = 0;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const int ping_ms = http_client.MeasureConnectLatencyMs(url);
        if (ping_ms >= 0)
        {
            total_ms += ping_ms;
            ++successful;
        }
    }

    return successful == 0
        ? -1
        : static_cast<int>((total_ms + successful / 2) / successful);
}

} // namespace

std::vector<StreamRegion> GfnClient::FetchStreamRegions(AuthSession& session) const
{
    session = RecoverSavedSession(session);
    const std::string base_url =
        server_location::ResolveStreamingBaseUrl("Auto", session.provider.streaming_service_url);
    if (base_url.empty())
        throw std::runtime_error("The GeForce NOW provider has no valid streaming endpoint.");

    const HttpResponse response = http_client_.Get(
        base_url + "v2/serverInfo",
        kUserAgent,
        BuildRegionHeaders(ResolveSessionJwt(session)));
    if (response.status_code != 200)
    {
        throw std::runtime_error(
            "Could not load GeForce NOW server locations (HTTP " +
            std::to_string(response.status_code) + ").");
    }

    JsonPtr root = LoadJson(response.body);
    json_t* metadata = json_object_get(root.get(), "metaData");
    if (!json_is_array(metadata))
        return {};

    std::vector<StreamRegion> regions;
    std::unordered_set<std::string> seen_urls;
    size_t index = 0;
    json_t* entry = nullptr;
    json_array_foreach(metadata, index, entry)
    {
        const std::string name = Trim(GetString(entry, "key"));
        const std::string raw_url = Trim(GetString(entry, "value"));
        if (name.empty() || name == "gfn-regions" || name.rfind("gfn-", 0) == 0)
            continue;

        const std::string url = server_location::NormalizeStreamingBaseUrl(raw_url);
        if (url.empty() || !seen_urls.insert(url).second)
            continue;

        regions.push_back({name, url, -1});
    }

    std::sort(regions.begin(), regions.end(), [](const StreamRegion& left, const StreamRegion& right) {
        return left.name < right.name;
    });
    return regions;
}

std::vector<StreamRegion> GfnClient::MeasureStreamRegionLatencies(
    std::vector<StreamRegion> regions) const
{
    if (regions.empty())
        return regions;

    std::atomic<size_t> next_index {0};
    const size_t worker_count = std::min<size_t>(4, regions.size());
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t worker = 0; worker < worker_count; ++worker)
    {
        workers.emplace_back([this, &regions, &next_index] {
            for (;;)
            {
                const size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
                if (index >= regions.size())
                    return;
                regions[index].ping_ms =
                    MeasureAverageLatency(http_client_, regions[index].url);
            }
        });
    }

    for (std::thread& worker : workers)
        worker.join();
    return regions;
}

} // namespace opennow
