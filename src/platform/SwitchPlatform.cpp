#include "platform/SwitchPlatform.hpp"

#if defined(OPENNOW_PLATFORM_SWITCH)
#include <switch.h>
#endif

#include <cstdio>

namespace opennow {

SwitchPlatform::SwitchPlatform() {
#if defined(OPENNOW_PLATFORM_SWITCH)
    consoleInit(nullptr);
#endif
}

SwitchPlatform::~SwitchPlatform() {
#if defined(OPENNOW_PLATFORM_SWITCH)
    consoleExit(nullptr);
#endif
}

std::string SwitchPlatform::name() const {
    return "nintendo-switch";
}

bool SwitchPlatform::isAppletMode() const {
#if defined(OPENNOW_PLATFORM_SWITCH)
    return appletGetAppletType() == AppletType_LibraryApplet;
#else
    return false;
#endif
}

void SwitchPlatform::clear() {
#if defined(OPENNOW_PLATFORM_SWITCH)
    consoleClear();
#endif
}

void SwitchPlatform::log(const std::string& message) {
    std::printf("%s\n", message.c_str());
#if defined(OPENNOW_PLATFORM_SWITCH)
    consoleUpdate(nullptr);
#endif
}

void SwitchPlatform::log(LogLevel level, LogCategory category, const std::string& message) {
    log(formatLogLine(level, category, message));
}

void SwitchPlatform::showScreen(const std::string& screen) {
    clear();
    std::printf("%s", screen.c_str());
#if defined(OPENNOW_PLATFORM_SWITCH)
    consoleUpdate(nullptr);
#endif
}

void SwitchPlatform::waitForExit() {
#if defined(OPENNOW_PLATFORM_SWITCH)
    log("");
    log("Press PLUS (+) to exit.");

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        const auto down = padGetButtonsDown(&pad);
        if ((down & HidNpadButton_Plus) != 0) {
            break;
        }
        consoleUpdate(nullptr);
    }
#endif
}

} // namespace opennow
