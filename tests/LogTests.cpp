#include "opennow/core/Log.hpp"

#include <cassert>
#include <string>

int main() {
    assert(opennow::toString(opennow::LogLevel::Info) == "INFO");
    assert(opennow::toString(opennow::LogLevel::Warning) == "WARN");
    assert(opennow::toString(opennow::LogCategory::Auth) == "AUTH");

    const auto line = opennow::formatLogLine(
        opennow::LogLevel::Error,
        opennow::LogCategory::Session,
        "CloudMatch failed");
    assert(line == "[ERROR][SESSION] CloudMatch failed");

    return 0;
}
