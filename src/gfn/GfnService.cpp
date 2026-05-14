#include "opennow/gfn/GfnService.hpp"

#include <sstream>

namespace opennow {

namespace {
constexpr const char* kGfnUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36 "
    "NVIDIACEFClient/HEAD/debb5919f6 GFN-PC/2.0.80.173";
constexpr const char* kGfnClientVersion = "2.0.80.173";
constexpr const char* kGfnClientId = "ec7e38d4-03af-4b58-b131-cfb0495903ab";
}

GfnCapabilityReport GfnService::capabilityReport(const StreamSettings& settings) const {
    std::ostringstream message;
    message
        << "GFN service boundary configured for " << describe(settings)
        << ". Native OAuth/token exchange is available in the host build with --login; "
        << "WSS signaling and WebRTC media transport still need implementation.";

    return {
        GfnCapabilityStatus::BlockedByNativeMediaTransport,
        message.str(),
    };
}

const char* GfnService::userAgent() const {
    return kGfnUserAgent;
}

const char* GfnService::clientVersion() const {
    return kGfnClientVersion;
}

const char* GfnService::clientId() const {
    return kGfnClientId;
}

} // namespace opennow
