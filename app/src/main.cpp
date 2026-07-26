#ifdef __SWITCH__
#include <switch.h>
#include <sys/stat.h>
#endif

#include <curl/curl.h>

#include <borealis.hpp>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <string>
#include <utility>

#include "main_activity.hpp"
#include "app_paths.hpp"
#include "gfn_client.hpp"
#include "home_shortcut.hpp"
#include "localization.hpp"
#include "stream_diagnostics.hpp"
#include "stream_settings.hpp"

#ifdef __SWITCH__
namespace
{

void EnsureLogDirectory()
{
    opennow::PrepareAppStorage();
    // QR device authorization never needs an account password. Remove any
    // legacy quick-login vault left by older builds before the UI starts.
    opennow::GfnClient().ClearAllNativeCredentials();
}

void AppendBootLog(const std::string& line)
{
    EnsureLogDirectory();

    std::ofstream stream(opennow::AppHomePath() + "/boot.log", std::ios::app);
    if (!stream.is_open())
        return;

    stream << line << '\n';
}

void ShowStartupFailure(const std::string& message)
{
    AppendBootLog("startup failure: " + message);

    consoleInit(nullptr);
    consoleClear();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState pad;
    padInitializeDefault(&pad);

    printf("OpenNOW failed to start.\n\n");
    printf("%s\n\n", message.c_str());
    printf("A boot log was written to:\n");
    printf("sdmc:/switch/SwitchNOW/boot.log\n\n");
    printf("Press PLUS or B to exit.\n");

    while (appletMainLoop())
    {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if (down & (HidNpadButton_Plus | HidNpadButton_B))
            break;

        consoleUpdate(nullptr);
    }

    consoleExit(nullptr);
}

} // namespace
#endif

int main(int argc, char* argv[])
{
    if (argc > 0 && argv && argv[0])
        opennow::shortcut::SetExecutablePath(argv[0]);
    auto launch_request = opennow::shortcut::ReadLaunchRequest(argc, argv);

    opennow::PrepareAppStorage();
    AppendBootLog("boot: entered main()");

#ifdef __SWITCH__
    appletSetWirelessPriorityMode(AppletWirelessPriorityMode_OptimizedForWlan);
#endif

    // Raised to LOG_DEBUG below once debug_diagnostics is known.
    brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);

    try
    {
        AppendBootLog("boot: calling curl_global_init()");

        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            ShowStartupFailure("curl_global_init failed");
            return EXIT_FAILURE;
        }

        AppendBootLog("boot: curl_global_init ok");
        AppendBootLog("boot: calling brls::Application::init()");

        if (!brls::Application::init())
        {
            ShowStartupFailure("brls::Application::init returned false");
            curl_global_cleanup();
            return EXIT_FAILURE;
        }

        AppendBootLog("boot: brls::Application::init ok");
        AppendBootLog("boot: creating window");

        const opennow::StreamSettings startup_settings = opennow::LoadStreamSettings();
        opennow::SetInterfaceLanguage(startup_settings.interface_language);
        brls::Application::createWindow("OpenNOW");
        AppendBootLog("boot: window created");

        // Plus is an in-game Xbox Start/Guide input. Borealis otherwise binds
        // BUTTON_START globally to Application::quit() before StreamView sees it.
        brls::Application::setGlobalQuit(false);
        brls::Application::setFPSStatus(false);
        opennow::SetStreamDiagnosticsEnabled(
            startup_settings.debug_diagnostics);
        if (startup_settings.debug_diagnostics)
            brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);

        AppendBootLog("boot: pushing main activity");
        brls::Application::pushActivity(
            new opennow::MainActivity(std::move(launch_request)));
        AppendBootLog("boot: main activity pushed");

        while (brls::Application::mainLoop())
            ;

        AppendBootLog("boot: main loop exited");
        curl_global_cleanup();
        return EXIT_SUCCESS;
    }
    catch (const std::exception& ex)
    {
        ShowStartupFailure(std::string("Unhandled exception: ") + ex.what());
    }
    catch (...)
    {
        ShowStartupFailure("Unhandled non-standard exception");
    }

    curl_global_cleanup();
    return EXIT_FAILURE;
}
