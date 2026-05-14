#pragma once

#include <string>
#include <string_view>

namespace opennow {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
};

enum class LogCategory {
    App,
    Platform,
    Auth,
    Network,
    Session,
    Signaling,
    Media,
    Input,
    Build,
};

std::string_view toString(LogLevel level);
std::string_view toString(LogCategory category);
std::string formatLogLine(LogLevel level, LogCategory category, std::string_view message);

} // namespace opennow
