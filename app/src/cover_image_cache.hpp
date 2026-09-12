#pragma once

#include <borealis.hpp>

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace opennow
{

struct CoverImageCacheStats
{
    std::uint64_t bytes = 0;
    std::size_t files = 0;
};

// Synchronously returns cached remote image bytes, downloading once when
// necessary. Call from a worker thread.
std::string LoadCachedImageData(const std::string& image_url,
    const std::atomic_bool* cancelled = nullptr);
CoverImageCacheStats InspectCoverImageCache();
std::size_t ClearCoverImageCache();
void ShutdownCoverImageWorker();

class CachedImage : public brls::Image
{
  public:
    ~CachedImage() override;
    void SetUrl(const std::string& image_url, bool fallback);
    void draw(NVGcontext* vg, float x, float y, float width, float height,
        brls::Style style, brls::FrameContext* ctx) override;

  private:
    void ResetImage();
    void TrySubmit();
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::string pending_url_;
    std::uint64_t requested_generation_ = 0;
    std::chrono::steady_clock::time_point next_submission_ {};
};

void SetCachedCoverImage(CachedImage* image, const std::string& image_url);
void SetCachedAvatarImage(CachedImage* image, const std::string& image_url);

} // namespace opennow
