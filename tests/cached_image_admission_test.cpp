#include "cover_image_cache.hpp"
#include "http_client.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
std::atomic<int> requests = 0;
std::promise<void> first_started;
std::promise<void> first_release;
const auto first_signal = first_release.get_future().share();

void Draw(opennow::CachedImage& image)
{
    image.draw(nullptr, 0, 0, 100, 100, {}, nullptr);
}
}

namespace opennow
{
const std::string& AppHomePath()
{
    static const std::string path =
        (std::filesystem::temp_directory_path() /
         ("opennow-admission-test-" + std::to_string(getpid()))).string();
    return path;
}

HttpResponse HttpClient::Get(
    const std::string& url, const std::string&, const std::vector<std::string>&,
    const std::string&, HttpTransferControl) const
{
    ++requests;
    if (url == "https://example.test/0")
    {
        first_started.set_value();
        first_signal.wait();
    }
    if (url == "https://example.test/failed")
        throw std::runtime_error("transfer failed");
    if (url == "https://example.test/unavailable")
        return {.status_code = 503, .body = {}};
    return {.status_code = 200, .body = "artwork"};
}
}

int main()
{
    using namespace opennow;
    std::filesystem::remove_all(AppHomePath());
    std::vector<std::unique_ptr<CachedImage>> images;
    for (int index = 0; index < 61; ++index)
    {
        auto image = std::make_unique<CachedImage>();
        image->SetUrl("https://example.test/" + std::to_string(index), true);
        images.push_back(std::move(image));
        if (index == 0)
            first_started.get_future().wait();
    }
    assert(requests == 1);
    first_release.set_value();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool complete = false;
    while (!complete && std::chrono::steady_clock::now() < deadline)
    {
        complete = true;
        for (auto& image : images)
        {
            Draw(*image);
            complete = complete && image->getTexture() == 2;
        }
        brls::DrainSync();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(images[1]->getTexture() == 2);
    assert(complete);
    assert(requests == 61);

    for (const auto& url : {"https://example.test/failed", "https://example.test/unavailable"})
    {
        CachedImage failed;
        failed.SetUrl(url, true);
        brls::WaitForSync();
        brls::DrainSync();
        assert(failed.getTexture() == 1);
        assert(failed.resource_path == "icon/icon.jpg");
        assert(std::filesystem::is_regular_file("resources/" + failed.resource_path));
        assert(!failed.getFreeTexture());
        const int completed_requests = requests;
        for (int frame = 0; frame < 35; ++frame)
        {
            Draw(failed);
            brls::DrainSync();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(requests == completed_requests);
        failed.SetUrl("https://example.test/1", true);
        brls::WaitForSync();
        brls::DrainSync();
        assert(failed.getTexture() == 2);
        assert(failed.getFreeTexture());
    }
    ShutdownCoverImageWorker();
    images.clear();
    brls::DrainSync();
    std::filesystem::remove_all(AppHomePath());
}
