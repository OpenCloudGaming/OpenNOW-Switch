#include "settings_tab.hpp"

#include "app_state.hpp"
#include "app_version.hpp"
#include "cover_image_cache.hpp"
#include "localization.hpp"
#include "membership_tier_policy.hpp"
#include "membership_tier_style.hpp"
#include "subscription_display.hpp"

#include <vector>

namespace opennow
{

void SettingsTab::BuildAccountPage()
{
    const auto& state = AppState::Instance();
    const std::vector<AuthSession> saved_accounts = client_.LoadSavedSessions();
    auto* overview = MakeSection(
        "GeForce NOW account", "OpenNOW keeps your sign-in ready between launches.");
    if (!state.HasSession())
    {
        AddInfoLine(overview, "Status", "Not connected");
        AddInfoLine(overview, "Next step", "Connect an account below");
    }
    else
    {
        const AuthSession& session = *state.session();
        AddInfoLine(overview, "User", session.user.display_name);
        const std::string membership_tier = membership::DisplayLabel(
            session.user.membership_tier, session.user.membership_tier_verified);
        auto* membership_label = AddInfoLine(
            overview, "Membership", membership_tier);
        membership_label->setTextColor(membership::TextColor(membership_tier));
        if (session.subscription.available)
        {
            AddInfoLine(
                overview, "Play time",
                subscription::FormatTimeRemaining(session.subscription));
            AddInfoLine(
                overview, "Persistent storage",
                subscription::FormatStorageUsage(session.subscription));
        }
        AddInfoLine(overview, "Provider", session.provider.display_name);
    }
    AddInfoLine(overview, "Saved accounts", std::to_string(saved_accounts.size()));
    content_container_->addView(overview);

    auto* actions = MakeSection("Account actions", "These actions take effect immediately.");
    actions->addView(MakeActionRow(
        state.HasSession() && state.session()->reauthentication_required
            ? "Reconnect account"
            : "Add account",
        "Connect with NVIDIA or a GeForce NOW Alliance partner using a QR code.",
        state.HasSession() && state.session()->reauthentication_required ? "Reconnect" : "Add",
        [this](brls::View* view) { return BeginLogin(view); }));
    actions->addView(MakeActionRow(
        "Switch account", "Choose another saved profile.", "Choose",
        [this](brls::View* view) { return SwitchSavedAccount(view); }));
    if (state.HasSession())
    {
        actions->addView(MakeActionRow(
            "Remove active account", "Disconnect this account from this console.", "Remove",
            [this](brls::View* view) { return ClearSavedLogin(view); }, true));
    }
    content_container_->addView(actions);
}

void SettingsTab::BuildStreamPage()
{
    auto* location = MakeSection(
        "Server location",
        "Use automatic routing or choose a specific GeForce NOW data center.");
    location->addView(MakeOptionRow(
        "Location",
        "Locations are ranked by direct latency from this console.",
        [this] { return ServerLocationValue(); },
        [this](brls::View* view) { return ChooseServerLocation(view); }));
    location->addView(MakeActionRow(
        "Latency test",
        "Reload available locations and test each connection again.",
        "Refresh",
        [this](brls::View* view) { return RefreshServerLocations(view); }));
    content_container_->addView(location);

    auto* video = MakeSection(
        "Video",
        "Choose resolution, frame rate, bitrate and video processing separately.");
    video->addView(MakeOptionRow(
        "Resolution", "720p reduces load; 1080p improves detail.",
        [this] {
            return std::to_string(draft_settings_.width) + " x " +
                std::to_string(draft_settings_.height);
        },
        [this](brls::View* view) { return CycleResolution(view); }));
    video->addView(MakeOptionRow(
        "FPS", "60 FPS is smoother; 30 FPS is more resilient.",
        [this] { return std::to_string(draft_settings_.fps) + " FPS"; },
        [this](brls::View* view) { return CycleFrameRate(view); }));
    video->addView(MakeOptionRow(
        "Bitrate", "Higher values improve motion detail but need stronger Wi-Fi.",
        [this] {
            return std::to_string(draft_settings_.bitrate_kbps / 1000) + " Mbps";
        },
        [this](brls::View* view) { return CycleBitrate(view); }));
    AddInfoLine(video, "Encoder", "H.264");
    video->addView(MakeOptionRow(
        "Decoder", "Choose automatic fallback, hardware-only or software-only decode.",
        [this] {
            if (draft_settings_.video_backend == "NVDEC")
                return std::string("Hardware");
            if (draft_settings_.video_backend == "Software")
                return std::string("Software");
            return std::string("Auto");
        },
        [this](brls::View* view) { return CycleVideoBackend(view); }));
    video->addView(MakeOptionRow(
        "Picture processing",
        "Adaptive is recommended; Clarity sharpens motion; Original keeps the source unchanged.",
        [this] { return draft_settings_.image_quality_mode; },
        [this](brls::View* view) { return CycleImageQuality(view); }));
    content_container_->addView(video);

    auto* connection = MakeSection(
        "Connection",
        "Optional routing for NVIDIA catalog, session creation and queue requests.");
    connection->addView(MakeOptionRow(
        "Zortos community proxy",
        "Streaming, signaling and account authentication always stay direct.",
        [this] {
            if (community_proxy_provisioning_)
                return std::string("Connecting...");
            return draft_settings_.community_proxy_enabled
                ? std::string("Enabled")
                : std::string("Disabled");
        },
        [this](brls::View* view) { return ToggleCommunityProxy(view); }));
    content_container_->addView(connection);

    if (!server_locations_loaded_ && !server_locations_loading_)
        BeginServerLocationLoad(false, false);
}

void SettingsTab::BuildPreferencesPage()
{
    auto* game = MakeSection(
        "Game & controls",
        "Choices used when a new GeForce NOW session starts.");
    game->addView(MakeOptionRow(
        "Game language",
        "Used for menus, subtitles and audio when supported.",
        [this] { return GameLanguageLabel(draft_settings_.game_language); },
        [this](brls::View* view) { return ChooseGameLanguage(view); }));
    game->addView(MakeOptionRow(
        "Remember game graphics",
        "Keep graphics options changed inside supported games.",
        [this] {
            return draft_settings_.persist_game_settings
                ? std::string("Enabled")
                : std::string("Disabled");
        },
        [this](brls::View* view) { return TogglePersistGameSettings(view); }));
    game->addView(MakeOptionRow(
        "Console mode",
        "Ask games and launchers such as Steam to start in a gamepad-friendly Big Picture mode.",
        [this] {
            return draft_settings_.launch_in_console_mode
                ? std::string("Enabled")
                : std::string("Disabled");
        },
        [this](brls::View* view) { return ToggleConsoleMode(view); }));
    game->addView(MakeOptionRow(
        "Face buttons",
        "Choose Xbox positions or matching Switch labels.",
        [this] { return draft_settings_.controller_layout; },
        [this](brls::View* view) { return ToggleControllerLayout(view); }));
    content_container_->addView(game);

    auto* audio = MakeSection("Audio", "Keep stream audio simple and predictable.");
    audio->addView(MakeOptionRow(
        "Audio output", "Enable or mute GeForce NOW audio.",
        [this] { return draft_settings_.audio_enabled ? std::string("Enabled") : std::string("Muted"); },
        [this](brls::View* view) { return ToggleAudio(view); }));
    audio->addView(MakeOptionRow(
        "Volume boost", "Compensates for quiet stream audio.",
        [this] { return std::to_string(draft_settings_.audio_volume / 100) + "x"; },
        [this](brls::View* view) { return CycleAudioVolume(view); }));
    content_container_->addView(audio);
}

void SettingsTab::BuildAppPage()
{
    auto* language = MakeSection("Language", "Choose the language used by the launcher.");
    language->addView(MakeOptionRow(
        "App language", "Applies after saving.",
        [this] { return InterfaceLanguageLabel(draft_settings_.interface_language); },
        [this](brls::View* view) { return ChooseInterfaceLanguage(view); }));
    content_container_->addView(language);

    auto* interface = MakeSection(
        "Interface", "Choose what OpenNOW shows while you play.");
    interface->addView(MakeOptionRow(
        "Stream stats overlay",
        "Show stream FPS, received bitrate and network ping during a session.",
        [this] {
            return draft_settings_.stats_overlay_enabled
                ? std::string("Enabled")
                : std::string("Disabled");
        },
        [this](brls::View* view) { return ToggleStatsOverlay(view); }));
    content_container_->addView(interface);

    auto* shortcuts = MakeSection(
        "Switch HOME screen",
        "OpenNOW can install its own Horizon HOME application.");
    shortcuts->addView(MakeActionRow(
        "Add OpenNOW to HOME",
        "Create and install the OpenNOW forwarder to SD storage.",
        "Install",
        [this](brls::View* view) { return ShowHomeScreenHelp(view); }));
    content_container_->addView(shortcuts);

    const CoverImageCacheStats images = InspectCoverImageCache();
    auto* cache = MakeSection("Storage", "Cover artwork is cached to keep the library responsive.");
    AddInfoLine(cache, "Cached covers", std::to_string(images.files));
    AddInfoLine(cache, "Disk usage", FormatBytes(images.bytes));
    cache->addView(MakeActionRow(
        "Clear cover artwork", "Covers will download again when needed.", "Clear",
        [this](brls::View* view) { return ClearCoverCache(view); }, true));
    content_container_->addView(cache);

    auto* about = MakeSection("OpenNOW", "Native GeForce NOW client for Nintendo Switch.");
    AddInfoLine(about, "Version", kAppVersion);
    content_container_->addView(about);
}

} // namespace opennow
