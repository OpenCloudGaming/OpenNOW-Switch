#pragma once

#include "opennow/platform/Platform.hpp"

namespace opennow {

class MockPlatform final : public Platform {
public:
    std::string name() const override;
    bool isAppletMode() const override;
    void clear() override;
    void log(const std::string& message) override;
    void log(LogLevel level, LogCategory category, const std::string& message) override;
};

} // namespace opennow
