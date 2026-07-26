#include "cover_image_cache.hpp"

#include "app_paths.hpp"
#include "gfn_client.hpp"
#include "http_client.hpp"

#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace opennow
{
namespace
{

constexpr const char* kFallbackCoverRes = "img/opennow_switch_icon.jpg";

std::string ImageCachePath()
{
    return AppHomePath() + "/cache/images";
}

void EnsureImageCacheDirectory()
{
#ifdef __SWITCH__
    const std::string app_home = AppHomePath();
    const std::string cache_path = app_home + "/cache";
    mkdir("sdmc:/switch", 0777);
    mkdir(app_home.c_str(), 0777);
    mkdir(cache_path.c_str(), 0777);
    mkdir(ImageCachePath().c_str(), 0777);
#endif
}

std::string HashUrl(const std::string& url)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : url)
    {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }

    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

std::string CachePathForUrl(const std::string& url)
{
    return ImageCachePath() + "/" + HashUrl(url) + ".img";
}

bool ReadCachedImage(const std::string& path, std::string& data)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
        return false;

    data.assign(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
    return !data.empty();
}

void WriteCachedImage(const std::string& path, const std::string& data)
{
    EnsureImageCacheDirectory();

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
        return;

    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
}

} // namespace

void SetCachedRemoteImage(brls::Image* image, const std::string& image_url)
{
    if (!image)
        return;

    if (image_url.empty())
        return;

    // Keep the Image's default freeTexture=true state. Setting a shared
    // resource first changes it to cache-managed; replacing that resource
    // with setImageAsync() then leaks every downloaded GPU texture because
    // the memory texture is not registered in TextureCache.
    image->setImageAsync([image_url](auto ready) {
        brls::async(
            [image_url, ready]() {
                try
                {
                    const std::string data = LoadCachedImageData(image_url);
                    ready(data, data.size());
                }
                catch (...)
                {
                    ready({}, 0);
                }
            },
            false);
    });
}

std::string LoadCachedImageData(const std::string& image_url)
{
    if (image_url.empty())
        return {};

    const std::string cache_path = CachePathForUrl(image_url);
    std::string cached;
    if (ReadCachedImage(cache_path, cached))
        return cached;

    HttpClient http_client;
    const HttpResponse response = http_client.Get(
        image_url,
        GfnClient::kUserAgent,
        {"Accept: image/jpeg,image/png,image/*,*/*;q=0.8"});
    if (response.status_code != 200 || response.body.empty())
        return {};

    WriteCachedImage(cache_path, response.body);
    return response.body;
}

CoverImageCacheStats InspectCoverImageCache()
{
    CoverImageCacheStats stats;
    const std::string cache_path = ImageCachePath();
    DIR* dir = opendir(cache_path.c_str());
    if (!dir)
        return stats;

    while (dirent* entry = readdir(dir))
    {
        const std::string name = entry->d_name;
        if (name.empty() || name == "." || name == "..")
            continue;

        const std::string path = cache_path + "/" + name;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        ++stats.files;
        stats.bytes += static_cast<std::uint64_t>(st.st_size);
    }

    closedir(dir);
    return stats;
}

std::size_t ClearCoverImageCache()
{
    std::size_t removed = 0;
    const std::string cache_path = ImageCachePath();
    DIR* dir = opendir(cache_path.c_str());
    if (!dir)
        return removed;

    while (dirent* entry = readdir(dir))
    {
        const std::string name = entry->d_name;
        if (name.empty() || name == "." || name == "..")
            continue;

        const std::string path = cache_path + "/" + name;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        if (std::remove(path.c_str()) == 0)
            ++removed;
    }

    closedir(dir);
    return removed;
}

void SetCachedCoverImage(brls::Image* image, const std::string& image_url)
{
    if (!image)
        return;
    if (image_url.empty())
    {
        image->setImageFromRes(kFallbackCoverRes);
        return;
    }
    SetCachedRemoteImage(image, image_url);
}

void SetCachedAvatarImage(brls::Image* image, const std::string& image_url)
{
    SetCachedRemoteImage(image, image_url);
}

} // namespace opennow
