#pragma once

#include "opennow/core/Log.hpp"

#include <string>

namespace opennow {

class Platform {
public:
    virtual ~Platform() = default;

    virtual std::string name() const = 0;
    virtual bool isAppletMode() const = 0;
    virtual void clear() {}
    virtual void log(const std::string& message) = 0;
    virtual void log(LogLevel level, LogCategory category, const std::string& message) {
        log(formatLogLine(level, category, message));
    }
    virtual void showScreen(const std::string& screen) {
        clear();
        log(screen);
    }
    virtual void waitForExit() {}
};

} // namespace opennow
