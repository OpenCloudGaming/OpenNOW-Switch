#include "platform/MockPlatform.hpp"

#include <iostream>

namespace opennow {

std::string MockPlatform::name() const {
    return "desktop-mock";
}

bool MockPlatform::isAppletMode() const {
    return false;
}

void MockPlatform::clear() {}

void MockPlatform::log(const std::string& message) {
    std::cout << message << '\n';
}

void MockPlatform::log(LogLevel level, LogCategory category, const std::string& message) {
    std::cout << formatLogLine(level, category, message) << '\n';
}

} // namespace opennow
