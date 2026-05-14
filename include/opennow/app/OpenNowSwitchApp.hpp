#pragma once

#include <memory>

#include "opennow/core/Settings.hpp"
#include "opennow/gfn/GfnService.hpp"
#include "opennow/platform/Platform.hpp"

namespace opennow {

class OpenNowSwitchApp {
public:
    explicit OpenNowSwitchApp(std::unique_ptr<Platform> platform);

    int run();

private:
    void printStartupSummary() const;
    void printRuntimeCategories() const;

    std::unique_ptr<Platform> platform_;
    StreamSettings settings_;
    GfnService gfn_;
};

} // namespace opennow
