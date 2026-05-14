#pragma once

#include <string>

#include "opennow/core/Settings.hpp"

namespace opennow {

enum class GfnCapabilityStatus {
    Ready,
    BlockedByNativeMediaTransport,
};

struct GfnCapabilityReport {
    GfnCapabilityStatus status = GfnCapabilityStatus::BlockedByNativeMediaTransport;
    std::string message;
};

class GfnService {
public:
    GfnCapabilityReport capabilityReport(const StreamSettings& settings) const;

    const char* userAgent() const;
    const char* clientVersion() const;
    const char* clientId() const;
};

} // namespace opennow
