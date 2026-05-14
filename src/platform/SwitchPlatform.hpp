#pragma once

#include "opennow/platform/Platform.hpp"

namespace opennow {

class SwitchPlatform final : public Platform {
public:
    SwitchPlatform();
    ~SwitchPlatform() override;

    std::string name() const override;
    bool isAppletMode() const override;
    void clear() override;
    void log(const std::string& message) override;
    void log(LogLevel level, LogCategory category, const std::string& message) override;
    void showScreen(const std::string& screen) override;
    void waitForExit() override;
};

} // namespace opennow
