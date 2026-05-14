#pragma once

#include <cstdint>
#include <string>

namespace opennow::gfn {

struct OAuthCallbackResult {
    bool ok = false;
    std::string code;
    std::string error;
};

class OAuthCallbackServer {
public:
    OAuthCallbackResult waitForCode(std::uint16_t port, std::uint32_t timeoutMs) const;

    static OAuthCallbackResult parseRequestTarget(const std::string& requestTarget);
};

} // namespace opennow::gfn
