#include "opennow/core/Log.hpp"

#include <sstream>

namespace opennow {

std::string_view toString(LogLevel level) {
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARN";
    case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

std::string_view toString(LogCategory category) {
    switch (category) {
    case LogCategory::App: return "APP";
    case LogCategory::Platform: return "PLATFORM";
    case LogCategory::Auth: return "AUTH";
    case LogCategory::Network: return "NETWORK";
    case LogCategory::Session: return "SESSION";
    case LogCategory::Signaling: return "SIGNALING";
    case LogCategory::Media: return "MEDIA";
    case LogCategory::Input: return "INPUT";
    case LogCategory::Build: return "BUILD";
    }
    return "APP";
}

std::string formatLogLine(LogLevel level, LogCategory category, std::string_view message) {
    std::ostringstream out;
    out << '[' << toString(level) << "][" << toString(category) << "] " << message;
    return out.str();
}

} // namespace opennow
