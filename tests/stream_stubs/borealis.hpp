#pragma once

namespace brls {
struct Logger {
    template <typename... Args>
    static void warning(const char*, Args&&...) {}
};
}
