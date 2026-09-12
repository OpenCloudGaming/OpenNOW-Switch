#include "cover_image_cache.hpp"

#include "app_paths.hpp"
#include "atomic_file_replace.hpp"
#include "gfn_client.hpp"
#include "http_client.hpp"
#include "cover_image_worker.hpp"

#include <borealis/core/cache_helper.hpp>

#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>

namespace opennow
{
namespace
{

constexpr const char* kFallbackCoverRes = "icon/icon.jpg";
std::mutex cache_mutex;
std::uint64_t cache_generation = 0;
std::unique_ptr<CoverImageWorker> image_worker;
constexpr std::size_t kMaxImageBytes = 8 * 1024 * 1024;

std::string LoadImageData(const std::string& image_url, const std::atomic_bool* cancelled,
    std::optional<std::uint64_t> requested_generation);

std::string ImageCachePath()
{
    return AppHomePath() + "/cache/images";
}

void EnsureImageCacheDirectory()
{
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
#endif
    const std::string app_home = AppHomePath();
    const std::string cache_path = app_home + "/cache";
    mkdir(app_home.c_str(), 0777);
    mkdir(cache_path.c_str(), 0777);
    mkdir(ImageCachePath().c_str(), 0777);
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

    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(kMaxImageBytes))
        return false;
    stream.seekg(0);

    data.assign(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
    return !data.empty();
}

void WriteCachedImage(const std::string& path, const std::string& data)
{
    EnsureImageCacheDirectory();

    const std::string temporary_path = path + ".tmp";
    std::ofstream stream(temporary_path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
        return;

    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    stream.close();
    if (!stream)
    {
        std::remove(temporary_path.c_str());
        return;
    }
    storage::ReplaceWithTemporaryFile(temporary_path, path);
}

} // namespace

CachedImage::~CachedImage()
{
    if (cancelled_)
        cancelled_->store(true);
}

void CachedImage::SetUrl(const std::string& image_url, bool fallback)
{
    if (cancelled_)
        cancelled_->store(true);
    pending_url_ = image_url;
    ResetImage();
    if (fallback || !image_url.empty())
        setImageFromRes(kFallbackCoverRes);
    if (image_url.empty())
        return;

    if (!image_worker)
        image_worker = std::make_unique<CoverImageWorker>();
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        requested_generation_ = cache_generation;
    }
    TrySubmit();
}

void CachedImage::ResetImage()
{
    if (!getFreeTexture() && getTexture() != 0)
        brls::TextureCache::instance().removeCache(getTexture());
    clear();
    setFreeTexture(true);
}

void CachedImage::draw(NVGcontext* vg, float x, float y, float width, float height,
    brls::Style style, brls::FrameContext* ctx)
{
    if (!pending_url_.empty() && std::chrono::steady_clock::now() >= next_submission_)
        TrySubmit();
    brls::Image::draw(vg, x, y, width, height, style, ctx);
}

void CachedImage::TrySubmit()
{
    next_submission_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    const auto cancelled = cancelled_;
    const auto generation = requested_generation_;
    const bool accepted = image_worker->TrySubmit(cancelled,
        [this, cancelled, image_url = pending_url_, generation] {
            std::string data;
            try
            {
                data = LoadImageData(image_url, cancelled.get(), generation);
            }
            catch (...)
            {
            }
            if (cancelled->load())
                return;
            brls::sync([this, cancelled, data] {
                if (cancelled->load() || data.empty())
                    return;
                ResetImage();
                setImageFromMem(reinterpret_cast<const unsigned char*>(data.data()),
                    static_cast<int>(data.size()));
                if (getTexture() == 0)
                    setImageFromRes(kFallbackCoverRes);
            });
        });
    if (accepted)
        pending_url_.clear();
}

void ShutdownCoverImageWorker()
{
    if (image_worker)
        image_worker->Stop();
}

std::string LoadCachedImageData(const std::string& image_url, const std::atomic_bool* cancelled)
{
    return LoadImageData(image_url, cancelled, std::nullopt);
}

namespace
{
std::string LoadImageData(const std::string& image_url, const std::atomic_bool* cancelled,
    std::optional<std::uint64_t> requested_generation)
{
    if (image_url.empty() || (cancelled && cancelled->load()))
        return {};

    const std::string cache_path = CachePathForUrl(image_url);
    std::string cached;
    std::uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        generation = cache_generation;
        if (requested_generation && *requested_generation != generation)
            return {};
        if (ReadCachedImage(cache_path, cached))
            return cached;
    }

    HttpClient http_client;
    const HttpResponse response = http_client.Get(
        image_url,
        GfnClient::kUserAgent,
        {"Accept: image/jpeg,image/png,image/*,*/*;q=0.8"}, {},
        {.cancelled = cancelled, .max_body_bytes = kMaxImageBytes});
    if (response.status_code != 200 || response.body.empty())
        return {};

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (generation == cache_generation && !(cancelled && cancelled->load()))
            WriteCachedImage(cache_path, response.body);
    }
    return response.body;
}
}

CoverImageCacheStats InspectCoverImageCache()
{
    std::lock_guard<std::mutex> lock(cache_mutex);
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
    std::lock_guard<std::mutex> lock(cache_mutex);
    ++cache_generation;
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

void SetCachedCoverImage(CachedImage* image, const std::string& image_url)
{
    if (image)
        image->SetUrl(image_url, true);
}

void SetCachedAvatarImage(CachedImage* image, const std::string& image_url)
{
    if (image)
        image->SetUrl(image_url, false);
}

} // namespace opennow
