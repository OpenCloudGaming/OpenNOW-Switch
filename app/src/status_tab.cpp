#include "status_tab.hpp"

#include "app_state.hpp"
#include "ui_helpers.hpp"
#include "localization.hpp"

namespace opennow
{
namespace
{

brls::Label* MakeParagraph(const std::string& text, float bottom_margin = 16.0f)
{
    auto* label = new brls::Label();
    label->setText(Tr(text));
    label->setFontSize(18);
    label->setMarginBottom(bottom_margin);
    return label;
}

} // namespace

StatusTab::StatusTab()
    : brls::Box(brls::Axis::COLUMN)
{
    setPadding(28, 40, 28, 40);

    auto* header = new brls::Header();
    header->setTitle("SwitchNOW");
    header->setSubtitle("Native Nintendo Switch homebrew port");
    addView(header);

    addView(MakeParagraph(
        "This port rebuilds OpenNOW as a native Switch client instead of trying to package the Electron app as an NRO."));

    addView(MakeParagraph(
        "Implemented now: persistent multi-account login with automatic token refresh, provider discovery, catalog and library sync, resilient CloudMatch lifecycle, and native WebRTC streaming."));

    addView(MakeParagraph(
        "Streaming now includes GFN signaling, WebRTC setup, FFmpeg/OpenGL video, diagnostics, FPS display, and Xbox-compatible input transport."));

    addView(MakeParagraph(
        "Still in progress: audio playback, rumble, microphone, and broader network compatibility.", 24.0f));

    auto* architecture_button = new brls::Button();
    architecture_button->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    architecture_button->setText("Why the port is staged this way");
    architecture_button->setMarginBottom(14);
    architecture_button->registerClickAction([this](brls::View* view) {
        return OpenArchitectureDialog(view);
    });
    addView(architecture_button);

    auto* cache_button = new brls::Button();
    cache_button->setText("Show current cache state");
    cache_button->registerClickAction([](brls::View* view) {
        const auto& state = AppState::Instance();
        ShowDialog(
            "Shared Cache",
            "Providers cached: " + std::string(state.HasProviders() ? "yes" : "no") + "\n" +
                "Public catalog cached: " + std::string(state.HasPublicGames() ? "yes" : "no") + "\n" +
                "Session loaded: " + std::string(state.IsSessionLoaded() ? "yes" : "no") + "\n" +
                "Signed in: " + std::string(state.HasSession() ? "yes" : "no") + "\n" +
                "Library games cached: " + std::to_string(state.library_games().size()));
        return true;
    });
    addView(cache_button);
}

bool StatusTab::OpenArchitectureDialog(brls::View* view)
{
    ShowDialog(
        "Port Boundary",
        "OpenNOW desktop is split across Electron UI, GFN API/session orchestration, and Chromium/native streaming.\n\n"
        "This Switch prototype ports the reusable GFN-facing layer first, replaces the desktop UI with Borealis, and now experiments with a native streaming backend.\n\n"
        "A real playable Switch client still needs robust Switch-native media transport, decode, and input behavior.");
    return true;
}

} // namespace opennow
