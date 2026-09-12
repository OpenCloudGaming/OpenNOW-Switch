#include "cover_image_cache.hpp"
#include "http_client.hpp"
#include <borealis/core/cache_helper.hpp>

#include <atomic>
#include <cassert>
#include <filesystem>
#include <future>
#include <string>
#include <unistd.h>

namespace
{
std::atomic<int> requests = 0;
std::promise<void> download_started;
std::promise<void> download_release;
const auto release_signal = download_release.get_future().share();
std::promise<void> cover_started;
std::promise<void> cover_release;
const auto cover_signal = cover_release.get_future().share();
}

namespace opennow
{
const std::string& AppHomePath()
{
    static const std::string path =
        (std::filesystem::temp_directory_path() /
         ("opennow-cover-test-" + std::to_string(getpid()))).string();
    return path;
}

HttpResponse HttpClient::Get(
    const std::string& url, const std::string&, const std::vector<std::string>&,
    const std::string&, HttpTransferControl control) const
{
    assert(control.max_body_bytes == 8 * 1024 * 1024);
    ++requests;
    if (url == "https://example.test/in-flight")
    {
        download_started.set_value();
        release_signal.wait();
    }
    if (url == "https://example.test/blocked-cover")
    {
        cover_started.set_value();
        cover_signal.wait();
    }
    return {.status_code = 200, .body = std::string(65536, 'x')};
}
}

int main()
{
    using namespace opennow;
    std::filesystem::remove_all(AppHomePath());
    assert(LoadCachedImageData("").empty());
    assert(requests == 0);
    const auto data = LoadCachedImageData("https://example.test/cached");
    assert(data.size() == 65536);
    assert(LoadCachedImageData("https://example.test/cached") == data);
    assert(requests == 1);
    assert(InspectCoverImageCache().files == 1);
    assert(InspectCoverImageCache().bytes == data.size());
    for (const auto& file : std::filesystem::directory_iterator(AppHomePath() + "/cache/images"))
        assert(file.path().extension() == ".img");

    auto download = std::async(std::launch::async, [] {
        return LoadCachedImageData("https://example.test/in-flight");
    });
    download_started.get_future().wait();
    assert(ClearCoverImageCache() == 1);
    assert(InspectCoverImageCache().files == 0);
    download_release.set_value();
    assert(download.get() == data);
    assert(InspectCoverImageCache().files == 0);
    assert(LoadCachedImageData("https://example.test/cached") == data);
    assert(requests == 3);
    assert(ClearCoverImageCache() == 1);
    assert(ClearCoverImageCache() == 0);

    {
        auto image = std::make_unique<CachedImage>();
        image->SetUrl("https://example.test/stale-image", false);
        brls::WaitForSync();
        image.reset();
        brls::DrainSync();
        assert(brls::image_uploads == 0);
    }
    {
        CachedImage image;
        const int previous_releases = brls::TextureCache::instance().releases;
        image.SetUrl("https://example.test/stale-image", false);
        brls::WaitForSync();
        image.SetUrl("", true);
        brls::DrainSync();
        assert(brls::image_uploads == 0);
        assert(!image.getFreeTexture());
        image.SetUrl("https://example.test/stale-image", false);
        brls::WaitForSync();
        brls::DrainSync();
        assert(brls::image_uploads == 1);
        assert(image.getFreeTexture());
        assert(brls::TextureCache::instance().releases == previous_releases + 3);
    }
    {
        CachedImage active;
        active.SetUrl("https://example.test/blocked-cover", false);
        cover_started.get_future().wait();
        CachedImage stale;
        stale.SetUrl("https://example.test/queued-before-clear", false);
        const int before = requests;
        ClearCoverImageCache();
        CachedImage fresh;
        fresh.SetUrl("https://example.test/queued-after-clear", false);
        cover_release.set_value();
        while (fresh.getTexture() != 2)
        {
            brls::WaitForSync();
            brls::DrainSync();
        }
        assert(requests == before + 1);
        assert(stale.getTexture() == 1);
        assert(stale.resource_path == "icon/icon.jpg");
        assert(InspectCoverImageCache().files == 1);
    }
    ShutdownCoverImageWorker();
    std::filesystem::remove_all(AppHomePath());
}
