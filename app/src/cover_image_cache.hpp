#pragma once

#include <borealis.hpp>

#include <string>

namespace opennow
{

// Synchronously returns cached remote image bytes, downloading once when
// necessary. Call from a worker thread.
std::string LoadCachedImageData(const std::string& image_url);
void SetCachedCoverImage(brls::Image* image, const std::string& image_url);
void SetCachedAvatarImage(brls::Image* image, const std::string& image_url);

} // namespace opennow
