#pragma once

#include <optional>
#include <string>

#include "opennow/gfn/GfnTypes.hpp"

namespace opennow::gfn {

struct StoredAuthSession {
    bool ok = false;
    AuthSession session;
    std::string error;
};

std::string defaultAuthSessionPath();
bool saveAuthSession(const AuthSession& session, const std::string& path, std::string& error);
StoredAuthSession loadAuthSession(const std::string& path);

} // namespace opennow::gfn
