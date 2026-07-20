#pragma once

#include <borealis.hpp>

#include <string>

namespace opennow
{

void SetCachedCoverImage(brls::Image* image, const std::string& image_url);
void SetCachedAvatarImage(brls::Image* image, const std::string& image_url);

} // namespace opennow
