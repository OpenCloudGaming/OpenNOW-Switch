#pragma once

#include "models.hpp"

#include <string>

namespace opennow
{

// Prefer the identity-provider picture and use the same deterministic
// Gravatar fallback as the desktop OpenNOW client.
std::string ResolveAvatarUrl(const AuthUser& user);
std::string ResolveAvatarUrl(const std::string& email, const std::string& picture_url = {});

} // namespace opennow
