#include "settings_tab.hpp"

#include "app_state.hpp"
#include "app_paths.hpp"
#include "localization.hpp"
#include "membership_tier_policy.hpp"
#include "network_utils.hpp"
#include "stream_settings.hpp"
#include "stream_settings_policy.hpp"
#include "stream_diagnostics.hpp"
#include "ui_helpers.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <cstdint>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <utility>
#include <vector>

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

const std::string kImageCachePath = AppHomePath() + "/cache/images";
const std::string kAppHomePath = AppHomePath();

struct ImageCacheInfo
{
    std::uint64_t bytes = 0;
    size_t files        = 0;
};

std::string FormatBytes(std::uint64_t bytes)
{
    if (bytes < 1024)
        return std::to_string(bytes) + " B";

    const double kib = static_cast<double>(bytes) / 1024.0;
    if (kib < 1024.0)
        return std::to_string(static_cast<int>(kib)) + " KiB";

    const double mib = kib / 1024.0;
    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(1) << mib << " MiB";
    return formatted.str();
}

std::string FormatTokenState(const AuthSession& session)
{
    if (session.reauthentication_required)
        return "reconnect required";

    if (!session.tokens.refresh_token.empty())
        return "connected; automatic renewal enabled";

    if (session.tokens.expires_at_ms <= 0)
        return "connected for this launch";

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto remaining_minutes = (session.tokens.expires_at_ms - now_ms) / 60000;
    if (remaining_minutes <= 0)
        return "expired; reconnect required";
    if (remaining_minutes < 60)
        return "temporary login valid for " + std::to_string(remaining_minutes) + " min";
    return "temporary login valid for " + std::to_string(remaining_minutes / 60) + " h";
}

ImageCacheInfo ReadImageCacheInfo()
{
    ImageCacheInfo info;

    DIR* dir = opendir(kImageCachePath.c_str());
    if (!dir)
        return info;

    while (dirent* entry = readdir(dir))
    {
        const std::string name = entry->d_name;
        if (name.empty() || name == "." || name == "..")
            continue;

        const std::string path = kImageCachePath + "/" + name;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        ++info.files;
        info.bytes += static_cast<std::uint64_t>(st.st_size);
    }

    closedir(dir);
    return info;
}

size_t ClearImageCacheFiles()
{
    size_t removed = 0;

    DIR* dir = opendir(kImageCachePath.c_str());
    if (!dir)
        return removed;

    while (dirent* entry = readdir(dir))
    {
        const std::string name = entry->d_name;
        if (name.empty() || name == "." || name == "..")
            continue;

        const std::string path = kImageCachePath + "/" + name;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        if (std::remove(path.c_str()) == 0)
            ++removed;
    }

    closedir(dir);
    return removed;
}

std::string AppletTypeName()
{
#ifdef __SWITCH__
    switch (appletGetAppletType())
    {
        case AppletType_Application:
            return "Application";
        case AppletType_SystemApplication:
            return "SystemApplication";
        case AppletType_LibraryApplet:
            return "LibraryApplet";
        case AppletType_OverlayApplet:
            return "OverlayApplet";
        case AppletType_SystemApplet:
            return "SystemApplet";
        default:
            return "Unknown";
    }
#else
    return "Non-Switch build";
#endif
}

std::string FileState(const std::string& path)
{
    struct stat st {};
    if (stat(path.c_str(), &st) != 0)
        return "missing";

    return std::to_string(static_cast<long long>(st.st_size)) + " bytes";
}

bool SameSettings(const StreamSettings& left, const StreamSettings& right)
{
    return left.preset_id == right.preset_id && left.label == right.label &&
           left.width == right.width && left.height == right.height &&
           left.fps == right.fps && left.bitrate_kbps == right.bitrate_kbps &&
           left.codec == right.codec && left.region == right.region &&
           left.audio_enabled == right.audio_enabled &&
           left.audio_volume == right.audio_volume &&
           left.audio_buffer_ms == right.audio_buffer_ms &&
           left.video_backend == right.video_backend &&
           left.debug_diagnostics == right.debug_diagnostics &&
           left.game_language == right.game_language &&
           left.persist_game_settings == right.persist_game_settings &&
           left.controller_layout == right.controller_layout &&
           left.image_quality_mode == right.image_quality_mode &&
           left.interface_language == right.interface_language;
}

} // namespace

SettingsTab::SettingsTab()
    : brls::Box(brls::Axis::COLUMN)
{
    setPadding(18, 28, 20, 28);

    auto* top = new brls::Box(brls::Axis::ROW);
    top->setHeight(72);
    top->setAlignItems(brls::AlignItems::CENTER);
    top->setMarginBottom(12);

    auto* title_column = new brls::Box(brls::Axis::COLUMN);
    title_column->setGrow(1.0f);
    auto* title = MakeParagraph("Settings", 3.0f);
    title->setFontSize(30);
    title_column->addView(title);
    auto* hint = MakeParagraph("Console-first controls. Press X to save, Y to revert.", 0.0f);
    hint->setFontSize(15);
    hint->setTextColor(nvgRGB(142, 149, 160));
    title_column->addView(hint);
    top->addView(title_column);

    save_status_ = MakeParagraph("All changes saved", 0.0f);
    save_status_->setFontSize(16);
    save_status_->setTextColor(nvgRGB(88, 230, 146));
    top->addView(save_status_);
    addView(top);

    auto* body = new brls::Box(brls::Axis::ROW);
    body->setGrow(1.0f);

    auto* sidebar = new brls::Box(brls::Axis::COLUMN);
    sidebar->setWidth(238);
    sidebar->setPadding(16, 14, 16, 14);
    sidebar->setMarginRight(18);
    sidebar->setCornerRadius(14);
    sidebar->setBackgroundColor(nvgRGB(15, 17, 21));

    auto* nav_label = MakeParagraph("CATEGORIES", 12.0f);
    nav_label->setFontSize(13);
    nav_label->setTextColor(nvgRGB(108, 115, 126));
    sidebar->addView(nav_label);

    const std::array<std::pair<const char*, Category>, 7> categories {{
        {"Account", Category::Account},
        {"Stream", Category::Stream},
        {"Game", Category::Game},
        {"Controls", Category::Controls},
        {"Audio", Category::Audio},
        {"Storage", Category::Storage},
        {"Interface", Category::Interface},
    }};
    for (const auto& [label, category] : categories)
    {
        auto* button = new brls::Button();
        button->setText(Tr(label));
        button->setHeight(58);
        button->setMarginBottom(10);
        button->setCornerRadius(10);
        button->registerClickAction([this, category](brls::View*) {
            SelectCategory(category);
            return true;
        });
        category_buttons_.push_back(button);
        sidebar->addView(button);
    }

    auto* nav_hint = MakeParagraph(
        "Changes apply to the next stream. Account and cache actions run immediately.", 0.0f);
    nav_hint->setFontSize(14);
    nav_hint->setTextColor(nvgRGB(124, 132, 144));
    nav_hint->setSingleLine(false);
    sidebar->addView(nav_hint);
    body->addView(sidebar);

    auto* content_shell = new brls::Box(brls::Axis::COLUMN);
    content_shell->setGrow(1.0f);
    content_shell->setPadding(0, 6, 0, 6);

    page_title_ = MakeParagraph("Stream", 2.0f);
    page_title_->setFontSize(27);
    content_shell->addView(page_title_);
    page_subtitle_ = MakeParagraph("Tune quality, latency and decoder behavior.", 12.0f);
    page_subtitle_->setFontSize(15);
    page_subtitle_->setTextColor(nvgRGB(142, 149, 160));
    content_shell->addView(page_subtitle_);

    scrolling_frame_ = new brls::ScrollingFrame();
    scrolling_frame_->setGrow(1.0f);
    scrolling_frame_->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    content_container_ = new brls::Box(brls::Axis::COLUMN);
    content_container_->setPadding(0, 0, 28, 0);
    scrolling_frame_->setContentView(content_container_);
    content_shell->addView(scrolling_frame_);
    body->addView(content_shell);
    addView(body);

    registerAction("Save", brls::BUTTON_X, [this](brls::View* view) {
        return SaveChanges(view);
    }, false, true);
    registerAction("Revert", brls::BUTTON_Y, [this](brls::View* view) {
        return RevertChanges(view);
    }, false, true);

    SelectCategory(Category::Stream);
}

brls::Box* SettingsTab::MakeSection(const std::string& title, const std::string& subtitle)
{
    auto* section = new brls::Box(brls::Axis::COLUMN);
    section->setPadding(18, 18, 18, 18);
    section->setMarginBottom(14);
    section->setCornerRadius(13);
    section->setBorderThickness(1);
    section->setBorderColor(nvgRGB(42, 46, 54));
    section->setBackgroundColor(nvgRGB(20, 22, 27));

    auto* label = MakeParagraph(title, subtitle.empty() ? 14.0f : 4.0f);
    label->setFontSize(21);
    label->setTextColor(nvgRGB(242, 244, 247));
    section->addView(label);
    if (!subtitle.empty())
    {
        auto* detail = MakeParagraph(subtitle, 14.0f);
        detail->setFontSize(14);
        detail->setTextColor(nvgRGB(133, 141, 152));
        section->addView(detail);
    }
    return section;
}

brls::Box* SettingsTab::MakeOptionRow(
    const std::string& title,
    const std::string& description,
    std::function<std::string()> value,
    std::function<bool(brls::View*)> action)
{
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setHeight(72);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setPadding(8, 12, 8, 12);
    row->setMarginBottom(8);
    row->setCornerRadius(10);
    row->setBackgroundColor(nvgRGB(25, 28, 34));

    auto* copy = new brls::Box(brls::Axis::COLUMN);
    copy->setGrow(1.0f);
    auto* name = MakeParagraph(title, 2.0f);
    name->setFontSize(18);
    copy->addView(name);
    auto* detail = MakeParagraph(description, 0.0f);
    detail->setFontSize(13);
    detail->setTextColor(nvgRGB(132, 140, 151));
    copy->addView(detail);
    row->addView(copy);

    auto* button = new brls::Button();
    button->setWidth(250);
    button->setHeight(50);
    button->setCornerRadius(9);
    button->setText(Tr(value()));
    button->registerClickAction([this, action = std::move(action)](brls::View* view) {
        const bool handled = action ? action(view) : true;
        UpdateOptionValues();
        return handled;
    });
    option_values_.push_back({button, std::move(value)});
    row->addView(button);
    return row;
}

brls::Box* SettingsTab::MakeActionRow(
    const std::string& title,
    const std::string& description,
    const std::string& button_text,
    std::function<bool(brls::View*)> action,
    bool destructive)
{
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setHeight(72);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setPadding(8, 12, 8, 12);
    row->setMarginBottom(8);
    row->setCornerRadius(10);
    row->setBackgroundColor(destructive ? nvgRGB(42, 24, 27) : nvgRGB(25, 28, 34));

    auto* copy = new brls::Box(brls::Axis::COLUMN);
    copy->setGrow(1.0f);
    auto* name = MakeParagraph(title, 2.0f);
    name->setFontSize(18);
    name->setTextColor(destructive ? nvgRGB(255, 170, 174) : nvgRGB(238, 241, 245));
    copy->addView(name);
    auto* detail = MakeParagraph(description, 0.0f);
    detail->setFontSize(13);
    detail->setTextColor(nvgRGB(132, 140, 151));
    copy->addView(detail);
    row->addView(copy);

    auto* button = new brls::Button();
    button->setWidth(250);
    button->setHeight(50);
    button->setCornerRadius(9);
    button->setText(Tr(button_text));
    button->registerClickAction(std::move(action));
    row->addView(button);
    return row;
}

void SettingsTab::AddInfoLine(brls::Box* parent, const std::string& label, const std::string& value)
{
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setHeight(40);
    row->setPadding(5, 8, 5, 8);
    row->setMarginBottom(2);
    row->setCornerRadius(7);
    row->setHighlightCornerRadius(7);
    row->setHighlightPadding(0);
    row->setFocusable(true);
    auto* key = MakeParagraph(label, 0.0f);
    key->setGrow(1.0f);
    key->setFontSize(15);
    key->setTextColor(nvgRGB(139, 147, 158));
    row->addView(key);
    auto* text = MakeParagraph(value, 0.0f);
    text->setFontSize(15);
    text->setTextColor(nvgRGB(210, 216, 225));
    row->addView(text);
    parent->addView(row);
}

void SettingsTab::SelectCategory(Category category)
{
    category_ = category;
    switch (category_)
    {
        case Category::Account:
            page_title_->setText(Tr("Account"));
            page_subtitle_->setText(Tr("Manage your GeForce NOW identity and persistent sign-in."));
            break;
        case Category::Stream:
            page_title_->setText(Tr("Stream"));
            page_subtitle_->setText(Tr("Balance image quality, latency and decoder stability."));
            break;
        case Category::Game:
            page_title_->setText(Tr("Game"));
            page_subtitle_->setText(Tr("Choose in-game localization and NVIDIA graphics persistence."));
            break;
        case Category::Controls:
            page_title_->setText(Tr("Controls"));
            page_subtitle_->setText(Tr("Choose whether face buttons follow Xbox positions or Switch labels."));
            break;
        case Category::Audio:
            page_title_->setText(Tr("Audio"));
            page_subtitle_->setText(Tr("Tune stream sound without changing the console volume."));
            break;
        case Category::Storage:
            page_title_->setText(Tr("Storage & system"));
            page_subtitle_->setText(Tr("Inspect cache usage, diagnostics and local app data."));
            break;
        case Category::Interface:
            page_title_->setText(Tr("Interface"));
            page_subtitle_->setText(Tr("Choose the launcher language."));
            break;
    }

    UpdateCategoryChrome();
    RebuildCategory();
}

void SettingsTab::UpdateCategoryChrome()
{
    const size_t selected = static_cast<size_t>(category_);
    for (size_t index = 0; index < category_buttons_.size(); ++index)
    {
        auto* button = category_buttons_[index];
        const bool active = index == selected;
        button->setBackgroundColor(active ? nvgRGB(22, 55, 42) : nvgRGB(24, 27, 32));
        button->setBorderThickness(active ? 2.0f : 0.0f);
        button->setBorderColor(active ? nvgRGB(74, 225, 142) : nvgRGB(24, 27, 32));
    }
}

void SettingsTab::RebuildCategory()
{
    if (!content_container_)
        return;

    option_values_.clear();
    content_container_->clearViews();
    switch (category_)
    {
        case Category::Account:
            BuildAccountPage();
            break;
        case Category::Stream:
            BuildStreamPage();
            break;
        case Category::Game:
            BuildGamePage();
            break;
        case Category::Controls:
            BuildControlsPage();
            break;
        case Category::Audio:
            BuildAudioPage();
            break;
        case Category::Storage:
            BuildStoragePage();
            break;
        case Category::Interface:
            BuildInterfacePage();
            break;
    }
    if (scrolling_frame_)
        scrolling_frame_->setContentOffsetY(0.0f, false);
    UpdateOptionValues();
}

void SettingsTab::BuildAccountPage()
{
    const auto& state = AppState::Instance();
    auto* overview = MakeSection("Connected account", "Authentication is renewed automatically when NVIDIA permits it.");
    if (!state.HasSession())
    {
        AddInfoLine(overview, "Status", "Not connected");
        AddInfoLine(overview, "Next step", "Sign in from Library");
    }
    else
    {
        const AuthSession& session = *state.session();
        AddInfoLine(overview, "User", session.user.display_name);
        AddInfoLine(
            overview, "Membership",
            membership::DisplayLabel(
                session.user.membership_tier, session.user.membership_tier_verified));
        AddInfoLine(overview, "Provider", session.provider.display_name);
        AddInfoLine(overview, "Authentication", FormatTokenState(session));
    }
    AddInfoLine(overview, "Saved accounts", std::to_string(client_.LoadSavedSessions().size()));
    content_container_->addView(overview);

    auto* session = MakeSection("Session", "Account actions take effect immediately and do not require X.");
    session->addView(MakeActionRow(
        "Session details", "Identity, membership, provider and token lifetime.", "View",
        [this](brls::View* view) { return ShowSessionDialog(view); }));
    session->addView(MakeActionRow(
        "Refresh authorization", "Renew the token and rotate the encrypted vault now.", "Refresh",
        [this](brls::View* view) { return TestTokenRefresh(view); }));
    session->addView(MakeActionRow(
        "Choose saved account", "Switch profiles without repeating sign-in.", "Choose",
        [this](brls::View* view) { return SwitchSavedAccount(view); }));
    content_container_->addView(session);

    auto* danger = MakeSection("Account removal", "These actions also clear the matching cached library state.");
    danger->addView(MakeActionRow(
        "Remove active account", "Disconnect the current account and select another saved profile.", "Remove",
        [this](brls::View* view) { return ClearSavedLogin(view); }, true));
    danger->addView(MakeActionRow(
        "Remove every account", "Delete all saved sessions from this console.", "Remove all",
        [this](brls::View* view) { return ClearAllSavedLogins(view); }, true));
    content_container_->addView(danger);
}

void SettingsTab::BuildStreamPage()
{
    auto* quality = MakeSection("Quality profile", "Start with a preset, then fine-tune individual values.");
    quality->addView(MakeOptionRow(
        "Preset", "Safe, Balanced or Quality baseline.",
        [this] { return draft_settings_.label; },
        [this](brls::View* view) { return CycleStreamPreset(view); }));
    quality->addView(MakeOptionRow(
        "Resolution", "720p reduces load; 1080p improves detail.",
        [this] { return std::to_string(draft_settings_.width) + " x " + std::to_string(draft_settings_.height); },
        [this](brls::View* view) { return CycleResolution(view); }));
    quality->addView(MakeOptionRow(
        "Frame rate", "60 FPS is smoother; 30 FPS is more resilient.",
        [this] { return std::to_string(draft_settings_.fps) + " FPS"; },
        [this](brls::View* view) { return CycleFrameRate(view); }));
    quality->addView(MakeOptionRow(
        "Maximum bitrate", "Higher values improve motion detail but need stronger Wi-Fi.",
        [this] { return std::to_string(draft_settings_.bitrate_kbps / 1000) + " Mbps"; },
        [this](brls::View* view) { return CycleBitrate(view); }));
    quality->addView(MakeOptionRow(
        "Motion clarity",
        "Adaptive balances recovery and detail; Clarity adds stronger cleanup; Original disables enhancement.",
        [this] { return draft_settings_.image_quality_mode; },
        [this](brls::View* view) { return CycleImageQuality(view); }));
    content_container_->addView(quality);

    auto* decoder = MakeSection("Decoder & delivery", "Auto uses Tegra X1 NVDEC and falls back to software when needed.");
    decoder->addView(MakeOptionRow(
        "Video backend", "Choose automatic fallback, hardware-only or software-only decode.",
        [this] { return draft_settings_.video_backend; },
        [this](brls::View* view) { return CycleVideoBackend(view); }));
    AddInfoLine(decoder, "Codec", "H.264, 8-bit 4:2:0");
    AddInfoLine(decoder, "Server region", draft_settings_.region + " (best latency)");
    content_container_->addView(decoder);

    auto* defaults = MakeSection("Recovery", "A known-good baseline for Nintendo Switch hardware.");
    defaults->addView(MakeActionRow(
        "Restore Balanced defaults", "720p, 60 FPS, 12 Mbps, automatic NVDEC fallback.", "Restore",
        [this](brls::View* view) { return ResetStreamPreset(view); }));
    content_container_->addView(defaults);

    auto* diagnostics = MakeSection(
        "Diagnostics",
        "Disabled by default to avoid file I/O and keep the stream screen clean.");
    diagnostics->addView(MakeOptionRow(
        "Debug diagnostics",
        "Show pre-stream telemetry and IN/DEC/OUT, and record detailed auth, session, stream, input, audio, render and UI logs.",
        [this] { return draft_settings_.debug_diagnostics ? std::string("Enabled") : std::string("Disabled"); },
        [this](brls::View* view) { return ToggleDebugDiagnostics(view); }));
    content_container_->addView(diagnostics);
}

void SettingsTab::BuildAudioPage()
{
    auto* output = MakeSection("Stream audio", "Audio remains synchronized with the currently presented video frame.");
    output->addView(MakeOptionRow(
        "Audio output", "Enable or mute GeForce NOW audio.",
        [this] { return draft_settings_.audio_enabled ? std::string("Enabled") : std::string("Muted"); },
        [this](brls::View* view) { return ToggleAudio(view); }));
    output->addView(MakeOptionRow(
        "Volume boost", "Compensates for NVIDIA's quiet decoded PCM level.",
        [this] { return std::to_string(draft_settings_.audio_volume / 100) + "x"; },
        [this](brls::View* view) { return CycleAudioVolume(view); }));
    output->addView(MakeOptionRow(
        "Playback buffer", "A larger buffer resists crackle but adds a little latency.",
        [this] { return std::to_string(draft_settings_.audio_buffer_ms) + " ms"; },
        [this](brls::View* view) { return CycleAudioBuffer(view); }));
    content_container_->addView(output);

    auto* format = MakeSection("Output format", "Selected automatically for Switch Audren.");
    AddInfoLine(format, "Source", "Opus stereo");
    AddInfoLine(format, "Mixer", "48 kHz PCM");
    AddInfoLine(format, "Synchronization", "RTCP video clock");
    content_container_->addView(format);
}

void SettingsTab::BuildGamePage()
{
    auto* game = MakeSection(
        "Game preferences",
        "These values are sent to NVIDIA when a new GeForce NOW session is created.");
    game->addView(MakeOptionRow(
        "Game language",
        "Language for in-game menus, subtitles and audio when the game supports it.",
        [this] { return GameLanguageLabel(draft_settings_.game_language); },
        [this](brls::View* view) { return ChooseGameLanguage(view); }));
    game->addView(MakeOptionRow(
        "Save in-game graphics settings",
        "Allow supported games to retain graphics options changed inside the stream.",
        [this] {
            return draft_settings_.persist_game_settings
                ? std::string("Enabled")
                : std::string("Disabled");
        },
        [this](brls::View* view) { return TogglePersistGameSettings(view); }));
    content_container_->addView(game);
}

void SettingsTab::BuildControlsPage()
{
    auto* layout = MakeSection(
        "Controller layout",
        "Only ABXY changes. Sticks, triggers, D-pad, hotkeys and system actions keep their current mapping.");
    layout->addView(MakeOptionRow(
        "Face buttons",
        "Xbox keeps the current positional layout; Switch makes physical labels match the remote buttons.",
        [this] { return draft_settings_.controller_layout; },
        [this](brls::View* view) { return ToggleControllerLayout(view); }));
    AddInfoLine(
        layout,
        "Xbox mode",
        "Switch A/B/X/Y -> Xbox B/A/Y/X");
    AddInfoLine(
        layout,
        "Switch mode",
        "Switch A/B/X/Y -> Xbox A/B/X/Y");
    content_container_->addView(layout);
}

void SettingsTab::BuildStoragePage()
{
    const auto& state = AppState::Instance();
    const ImageCacheInfo images = ReadImageCacheInfo();
    auto* cache = MakeSection("Cover cache", "Artwork is stored on the SD card to keep the library responsive.");
    AddInfoLine(cache, "Cover files", std::to_string(images.files));
    AddInfoLine(cache, "Disk usage", FormatBytes(images.bytes));
    AddInfoLine(cache, "Public catalog", state.HasPublicGames() ? "Loaded" : "Not loaded");
    AddInfoLine(cache, "Owned library", state.HasLibraryGames() ? "Loaded" : "Not loaded");
    cache->addView(MakeActionRow(
        "Inspect shared cache", "Show provider, catalog, library and artwork state.", "Inspect",
        [this](brls::View* view) { return ShowCacheState(view); }));
    cache->addView(MakeActionRow(
        "Clear cover artwork", "Remove downloaded covers; they will be fetched again on demand.", "Clear",
        [this](brls::View* view) { return ClearCoverCache(view); }, true));
    content_container_->addView(cache);

    auto* system = MakeSection("System", "Information useful when diagnosing a hardware or network problem.");
    AddInfoLine(system, "Applet mode", AppletTypeName());
    AddInfoLine(system, "Local IP", NetworkUtils::GetLocalIPAddress());
    system->addView(MakeActionRow(
        "Open diagnostics", "Inspect runtime mode, stream settings, logs and storage paths.", "Open",
        [this](brls::View* view) { return ShowDiagnostics(view); }));
    content_container_->addView(system);
}

void SettingsTab::BuildInterfacePage()
{
    auto* language = MakeSection(
        "Language",
        "Choose the launcher language.");
    language->addView(MakeOptionRow(
        "Language",
        "Choose the launcher language.",
        [this] { return InterfaceLanguageLabel(draft_settings_.interface_language); },
        [this](brls::View* view) { return ChooseInterfaceLanguage(view); }));
    content_container_->addView(language);

    auto* about = MakeSection("SwitchNOW", "Native GeForce NOW client for Nintendo Switch.");
    AddInfoLine(about, "Version", "1.0.0");
    AddInfoLine(about, "Languages", "English + 7");
    content_container_->addView(about);
}

void SettingsTab::UpdateOptionValues()
{
    for (auto& [button, value] : option_values_)
    {
        if (button && value)
            button->setText(Tr(value()));
    }
}

void SettingsTab::MarkDirty()
{
    dirty_ = !SameSettings(draft_settings_, saved_settings_);
    RefreshSummary();
}

bool SettingsTab::SaveChanges(brls::View* view)
{
    (void)view;
    if (!settings_loaded_ || !dirty_)
    {
        brls::Application::notify("Settings are already up to date");
        return true;
    }

    if (!SaveStreamSettings(draft_settings_))
    {
        ShowError("Settings Save Failed", "Could not safely write stream_settings.json to the SD card.");
        return true;
    }

    saved_settings_ = LoadStreamSettings();
    draft_settings_ = saved_settings_;
    SetInterfaceLanguage(saved_settings_.interface_language);
    SetStreamDiagnosticsEnabled(saved_settings_.debug_diagnostics);
    dirty_ = false;
    RefreshSummary();
    UpdateOptionValues();
    const std::array<const char*, 7> category_names {
        "Account", "Stream", "Game", "Controls", "Audio", "Storage", "Interface"};
    for (size_t index = 0; index < category_buttons_.size() && index < category_names.size(); ++index)
        category_buttons_[index]->setText(Tr(category_names[index]));
    SelectCategory(category_);
    brls::Application::notify(Tr("Settings saved; changes apply to the next stream"));
    return true;
}

bool SettingsTab::RevertChanges(brls::View* view)
{
    (void)view;
    draft_settings_ = saved_settings_;
    dirty_ = false;
    RefreshSummary();
    UpdateOptionValues();
    brls::Application::notify(Tr("Unsaved changes reverted"));
    return true;
}

bool SettingsTab::CycleResolution(brls::View* view)
{
    (void)view;
    settings::CycleResolution(draft_settings_);
    MarkDirty();
    return true;
}

bool SettingsTab::CycleFrameRate(brls::View* view)
{
    (void)view;
    settings::CycleFrameRate(draft_settings_);
    MarkDirty();
    return true;
}

bool SettingsTab::CycleBitrate(brls::View* view)
{
    (void)view;
    settings::CycleBitrate(draft_settings_);
    MarkDirty();
    return true;
}

void SettingsTab::willAppear(bool resetState)
{
    brls::Box::willAppear(resetState);
    EnsureSessionLoaded();
    if (!settings_loaded_)
    {
        saved_settings_ = LoadStreamSettings();
        draft_settings_ = saved_settings_;
        settings_loaded_ = true;
    }
    RefreshSummary();
    RebuildCategory();
}

void SettingsTab::EnsureSessionLoaded()
{
    auto& state = AppState::Instance();
    if (state.IsSessionLoaded())
        return;

    AuthSession session;
    if (client_.LoadSavedSession(session))
        state.SetSession(std::move(session));
    else
        state.MarkSessionLoaded();
}

void SettingsTab::RefreshSummary()
{
    if (!save_status_)
        return;

    if (dirty_)
    {
        save_status_->setText(Tr("Unsaved changes  |  X Save"));
        save_status_->setTextColor(nvgRGB(255, 190, 92));
    }
    else
    {
        save_status_->setText(Tr("All changes saved"));
        save_status_->setTextColor(nvgRGB(88, 230, 146));
    }
}

bool SettingsTab::ShowSessionDialog(brls::View* view)
{
    (void)view;

    const auto& state = AppState::Instance();
    if (!state.HasSession())
    {
        ShowDialog(
            "Session Details",
            "No saved GeForce NOW login is available on this Switch.");
        return true;
    }

    const AuthSession& session = *state.session();
    ShowDialog(
        "Session Details",
        "User: " + session.user.display_name + "\n" +
            "Email: " + (session.user.email.empty() ? std::string("Unavailable") : session.user.email) + "\n" +
            "Tier: " + membership::DisplayLabel(
                session.user.membership_tier, session.user.membership_tier_verified) + "\n" +
            "Provider: " + session.provider.display_name + "\n" +
            "Authentication: " + FormatTokenState(session) + "\n" +
            "Streaming base: " + session.provider.streaming_service_url + "\n" +
            "Saved accounts: " + std::to_string(client_.LoadSavedSessions().size()));
    return true;
}

bool SettingsTab::TestTokenRefresh(brls::View* view)
{
    (void)view;
    auto& state = AppState::Instance();
    if (!state.HasSession())
    {
        ShowDialog("Token Refresh Test", "No GeForce NOW account is connected.");
        return true;
    }

    try
    {
        AuthSession refreshed = client_.ForceRefreshSavedSession(*state.session());
        refreshed.reauthentication_required = false;
        state.SetSession(std::move(refreshed));
        RefreshSummary();
        brls::sync([this] { RebuildCategory(); });
        brls::Application::notify("Token refresh and vault rotation completed successfully");
    }
    catch (const ReauthenticationRequired& e)
    {
        AuthSession invalid = *state.session();
        invalid.reauthentication_required = true;
        state.SetSession(std::move(invalid));
        RefreshSummary();
        brls::sync([this] { RebuildCategory(); });
        ShowError("Token Refresh Requires Login", e.what());
    }
    catch (const std::exception& e)
    {
        ShowError("Token Refresh Test Failed", e.what());
    }
    return true;
}

bool SettingsTab::ClearSavedLogin(brls::View* view)
{
    (void)view;

    client_.ClearSavedSession();

    auto& state = AppState::Instance();
    state.SetLibraryGames({});

    AuthSession next;
    if (client_.LoadSavedSession(next))
    {
        state.SetSession(std::move(next));
        brls::Application::notify("Active login cleared; switched to another saved account");
    }
    else
    {
        state.ClearSession();
        brls::Application::notify("Saved GeForce NOW login cleared");
    }

    RefreshSummary();
    brls::sync([this] { RebuildCategory(); });
    return true;
}

bool SettingsTab::SwitchSavedAccount(brls::View* view)
{
    (void)view;

    std::vector<AuthSession> sessions = client_.LoadSavedSessions();
    if (sessions.empty())
    {
        ShowDialog("Saved Accounts", "No saved GeForce NOW accounts are available.");
        return true;
    }

    const std::string active_user_id =
        AppState::Instance().HasSession() ? AppState::Instance().session()->user.user_id : "";

    auto* dialog = new brls::Dialog("Choose the GeForce NOW account to use.");
    for (const AuthSession& session : sessions)
    {
        const bool active = session.user.user_id == active_user_id;
        const std::string label = (active ? "Active: " : "Use: ") + session.user.display_name;
        dialog->addButton(label, [this, session]() {
            if (!client_.SetActiveSavedSession(session.user.user_id))
            {
                ShowError("Account Switch Failed", "Unable to activate the selected saved account.");
                return;
            }

            auto& state = AppState::Instance();
            state.SetSession(session);
            state.SetLibraryGames({});
            RefreshSummary();
            brls::sync([this] { RebuildCategory(); });
            brls::Application::notify("Switched to " + session.user.display_name);
        });
    }
    dialog->addButton("Cancel", [] {});
    dialog->open();
    return true;
}

bool SettingsTab::ClearAllSavedLogins(brls::View* view)
{
    (void)view;

    client_.ClearAllSavedSessions();
    client_.ClearAllNativeCredentials();
    auto& state = AppState::Instance();
    state.ClearSession();
    state.SetLibraryGames({});
    RefreshSummary();
    brls::sync([this] { RebuildCategory(); });
    brls::Application::notify("All saved GeForce NOW logins cleared");
    return true;
}

bool SettingsTab::ShowCacheState(brls::View* view)
{
    (void)view;

    const auto& state = AppState::Instance();
    const ImageCacheInfo images = ReadImageCacheInfo();
    ShowDialog(
        "Shared Cache",
        "Providers cached: " + std::string(state.HasProviders() ? "yes" : "no") + "\n" +
            "Public catalog cached: " + std::string(state.HasPublicGames() ? "yes" : "no") + "\n" +
            "Library cached: " + std::string(state.HasLibraryGames() ? "yes" : "no") + "\n" +
            "Cover files: " + std::to_string(images.files) + "\n" +
            "Cover cache size: " + FormatBytes(images.bytes));
    return true;
}

bool SettingsTab::ClearCoverCache(brls::View* view)
{
    (void)view;

    const size_t removed = ClearImageCacheFiles();
    RefreshSummary();
    brls::sync([this] { RebuildCategory(); });
    brls::Application::notify("Removed " + std::to_string(removed) + " cached cover files");
    return true;
}

bool SettingsTab::CycleStreamPreset(brls::View* view)
{
    (void)view;
    const bool audio_enabled = draft_settings_.audio_enabled;
    const int audio_volume = draft_settings_.audio_volume;
    const int audio_buffer_ms = draft_settings_.audio_buffer_ms;
    const std::string backend = draft_settings_.video_backend;
    const bool debug_diagnostics = draft_settings_.debug_diagnostics;
    const std::string game_language = draft_settings_.game_language;
    const bool persist_game_settings = draft_settings_.persist_game_settings;
    const std::string controller_layout = draft_settings_.controller_layout;
    const std::string image_quality_mode = draft_settings_.image_quality_mode;
    const std::string interface_language = draft_settings_.interface_language;

    draft_settings_ = NextStreamPreset(draft_settings_);
    draft_settings_.audio_enabled = audio_enabled;
    draft_settings_.audio_volume = audio_volume;
    draft_settings_.audio_buffer_ms = audio_buffer_ms;
    draft_settings_.video_backend = backend;
    draft_settings_.debug_diagnostics = debug_diagnostics;
    draft_settings_.game_language = game_language;
    draft_settings_.persist_game_settings = persist_game_settings;
    draft_settings_.controller_layout = controller_layout;
    draft_settings_.image_quality_mode = image_quality_mode;
    draft_settings_.interface_language = interface_language;
    MarkDirty();
    return true;
}

bool SettingsTab::ResetStreamPreset(brls::View* view)
{
    (void)view;

    const bool debug_diagnostics = draft_settings_.debug_diagnostics;
    const std::string game_language = draft_settings_.game_language;
    const bool persist_game_settings = draft_settings_.persist_game_settings;
    const std::string controller_layout = draft_settings_.controller_layout;
    const std::string image_quality_mode = draft_settings_.image_quality_mode;
    const std::string interface_language = draft_settings_.interface_language;
    draft_settings_ = StreamPresets()[1];
    draft_settings_.debug_diagnostics = debug_diagnostics;
    draft_settings_.game_language = game_language;
    draft_settings_.persist_game_settings = persist_game_settings;
    draft_settings_.controller_layout = controller_layout;
    draft_settings_.image_quality_mode = image_quality_mode;
    draft_settings_.interface_language = interface_language;
    MarkDirty();
    UpdateOptionValues();
    brls::Application::notify("Balanced defaults staged; press X to save");
    return true;
}

bool SettingsTab::CycleVideoBackend(brls::View* view)
{
    (void)view;
    if (draft_settings_.video_backend == "Auto")
        draft_settings_.video_backend = "NVDEC";
    else if (draft_settings_.video_backend == "NVDEC")
        draft_settings_.video_backend = "Software";
    else
        draft_settings_.video_backend = "Auto";
    MarkDirty();
    return true;
}

bool SettingsTab::CycleImageQuality(brls::View* view)
{
    (void)view;
    settings::CycleImageQuality(draft_settings_);
    MarkDirty();
    return true;
}

bool SettingsTab::ChooseGameLanguage(brls::View* view)
{
    (void)view;
    const auto& options = GameLanguageOptions();
    std::vector<std::string> labels;
    labels.reserve(options.size());
    int selected = 0;
    for (size_t index = 0; index < options.size(); ++index)
    {
        labels.push_back(options[index].label);
        if (options[index].code == draft_settings_.game_language)
            selected = static_cast<int>(index);
    }

    auto* dropdown = new brls::Dropdown(
        "Game language", labels,
        [this](int index) {
            const auto& languages = GameLanguageOptions();
            if (index < 0 || static_cast<size_t>(index) >= languages.size())
                return;
            draft_settings_.game_language = languages[static_cast<size_t>(index)].code;
            MarkDirty();
            UpdateOptionValues();
        },
        selected);
    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}

bool SettingsTab::ChooseInterfaceLanguage(brls::View* view)
{
    (void)view;
    const auto& options = InterfaceLanguageOptions();
    std::vector<std::string> labels;
    labels.reserve(options.size());
    int selected = 0;
    for (size_t index = 0; index < options.size(); ++index)
    {
        labels.push_back(options[index].label);
        if (options[index].code == draft_settings_.interface_language)
            selected = static_cast<int>(index);
    }

    auto* dropdown = new brls::Dropdown(
        Tr("Language"), labels,
        [this](int index) {
            const auto& languages = InterfaceLanguageOptions();
            if (index < 0 || static_cast<size_t>(index) >= languages.size())
                return;
            draft_settings_.interface_language = languages[static_cast<size_t>(index)].code;
            MarkDirty();
            UpdateOptionValues();
        },
        selected);
    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}

bool SettingsTab::TogglePersistGameSettings(brls::View* view)
{
    (void)view;
    draft_settings_.persist_game_settings = !draft_settings_.persist_game_settings;
    MarkDirty();
    return true;
}

bool SettingsTab::ToggleControllerLayout(brls::View* view)
{
    (void)view;
    draft_settings_.controller_layout =
        draft_settings_.controller_layout == "Switch" ? "Xbox" : "Switch";
    MarkDirty();
    return true;
}

bool SettingsTab::ToggleAudio(brls::View* view)
{
    (void)view;
    draft_settings_.audio_enabled = !draft_settings_.audio_enabled;
    MarkDirty();
    return true;
}

bool SettingsTab::CycleAudioVolume(brls::View* view)
{
    (void)view;
    constexpr int gains[] = {800, 1000, 1200, 1400, 1600};
    int next_gain = gains[0];
    for (int gain : gains)
    {
        if (gain > draft_settings_.audio_volume)
        {
            next_gain = gain;
            break;
        }
    }
    draft_settings_.audio_volume = next_gain;
    MarkDirty();
    return true;
}

bool SettingsTab::CycleAudioBuffer(brls::View* view)
{
    (void)view;
    draft_settings_.audio_buffer_ms =
        draft_settings_.audio_buffer_ms >= 100 ? 30 : draft_settings_.audio_buffer_ms + 10;
    MarkDirty();
    return true;
}

bool SettingsTab::ToggleDebugDiagnostics(brls::View* view)
{
    (void)view;
    draft_settings_.debug_diagnostics = !draft_settings_.debug_diagnostics;
    MarkDirty();
    brls::Application::notify(
        draft_settings_.debug_diagnostics
            ? "Debug diagnostics staged; press X to save"
            : "Clean stream mode staged; press X to save");
    return true;
}

bool SettingsTab::ShowDiagnostics(brls::View* view)
{
    (void)view;

    const auto& state = AppState::Instance();
    const ImageCacheInfo images = ReadImageCacheInfo();
    const StreamSettings stream_settings = LoadStreamSettings();
    const std::string local_ip = NetworkUtils::GetLocalIPAddress();

    std::string account = "not connected";
    std::string provider = "n/a";
    if (state.HasSession())
    {
        account = state.session()->user.display_name;
        provider = state.session()->provider.display_name;
    }

    ShowDialog(
        "Diagnostics",
        "Applet type: " + AppletTypeName() + "\n" +
            "Local IP: " + local_ip + "\n" +
            "Account: " + account + "\n" +
            "Provider: " + provider + "\n" +
            "Providers cached: " + std::string(state.HasProviders() ? "yes" : "no") + "\n" +
            "Public catalog entries: " + std::to_string(state.public_games().size()) + "\n" +
            "Library entries: " + std::to_string(state.library_games().size()) + "\n" +
            "Stream: " + FormatStreamSettings(stream_settings) + "\n" +
            "Cover cache: " + std::to_string(images.files) + " files, " + FormatBytes(images.bytes) + "\n\n" +
            "Paths:\n" +
            std::string(kAppHomePath) + "\n" +
            "boot.log: " + FileState(std::string(kAppHomePath) + "/boot.log") + "\n" +
            "auth.log: " + FileState(std::string(kAppHomePath) + "/auth.log") + "\n" +
            "input.log: " + FileState(std::string(kAppHomePath) + "/input.log") + "\n" +
            "audio.log: " + FileState(std::string(kAppHomePath) + "/audio.log") + "\n" +
            "signaling.log: " + FileState(std::string(kAppHomePath) + "/signaling.log") + "\n" +
            "nte_autologin.log: " + FileState(std::string(kAppHomePath) + "/nte/nte_autologin.log") + "\n" +
            "auth_accounts.json: " + FileState(std::string(kAppHomePath) + "/auth_accounts.json") + "\n" +
            "active_cloud_session.json: " + FileState(std::string(kAppHomePath) + "/active_cloud_session.json") + "\n" +
            "stream_settings.json: " + FileState(std::string(kAppHomePath) + "/stream_settings.json") + "\n\n" +
            "Tip: NVIDIA login needs Application mode. If Applet type is not Application/SystemApplication, launch hbmenu with title override.");
    return true;
}

} // namespace opennow
