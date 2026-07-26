#pragma once

#include <borealis.hpp>

#include <cstddef>
#include <cstdint>
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
std::string LoadCachedImageData(const std::string& image_url);
CoverImageCacheStats InspectCoverImageCache();
std::size_t ClearCoverImageCache();
void SetCachedCoverImage(brls::Image* image, const std::string& image_url);
void SetCachedAvatarImage(brls::Image* image, const std::string& image_url);

} // namespace opennow
