#pragma once

namespace brls {
struct Logger {
    template <typename... Args>
    static void debug(const char*, Args&&...) {}
    template <typename... Args>
    static void info(const char*, Args&&...) {}
    template <typename... Args>
    static void error(const char*, Args&&...) {}
    template <typename... Args>
    static void warning(const char*, Args&&...) {}
};
}
