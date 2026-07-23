#include "settings_tab.hpp"

#include "app_state.hpp"
#include "app_paths.hpp"
#include "home_shortcut.hpp"
#include "localization.hpp"
#include "membership_tier_policy.hpp"
#include "qr_login_dialog.hpp"
#include "server_location_policy.hpp"
#include "stream_settings.hpp"
#include "stream_settings_policy.hpp"
#include "stream_diagnostics.hpp"
#include "subscription_display.hpp"
#include "ui_action_guard.hpp"
#include "ui_helpers.hpp"

#include <cstdint>
#include <algorithm>
#include <array>
#include <cstdio>
#include <dirent.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
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
           left.stats_overlay_enabled == right.stats_overlay_enabled &&
           left.game_language == right.game_language &&
           left.persist_game_settings == right.persist_game_settings &&
           left.controller_layout == right.controller_layout &&
           left.image_quality_mode == right.image_quality_mode &&
           left.interface_language == right.interface_language &&
           left.community_proxy_enabled == right.community_proxy_enabled &&
           left.community_proxy_url == right.community_proxy_url;
}

} // namespace

SettingsTab::SettingsTab()
    : brls::Box(brls::Axis::COLUMN)
{
    setPadding(16, 28, 18, 28);
    setBackgroundColor(nvgRGB(12, 13, 16));

    auto* top = new brls::Box(brls::Axis::ROW);
    top->setHeight(62);
    top->setAlignItems(brls::AlignItems::CENTER);
    top->setMarginBottom(12);

    auto* title_column = new brls::Box(brls::Axis::COLUMN);
    title_column->setGrow(1.0f);
    auto* title = MakeParagraph("Settings", 3.0f);
    title->setFontSize(30);
    title_column->addView(title);
    auto* hint = MakeParagraph("Account, streaming and app preferences in one place.", 0.0f);
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
    sidebar->setWidth(220);
    sidebar->setPadding(14, 12, 16, 12);
    sidebar->setBackgroundColor(nvgRGB(16, 18, 22));

    auto* nav_label = MakeParagraph("General", 10.0f);
    nav_label->setFontSize(13);
    nav_label->setTextColor(nvgRGB(108, 115, 126));
    sidebar->addView(nav_label);

    const std::array<std::pair<const char*, Category>, 4> categories {{
        {"Account", Category::Account},
        {"Stream", Category::Stream},
        {"Preferences", Category::Preferences},
        {"App", Category::App},
    }};
    for (const auto& [label, category] : categories)
    {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setHeight(44);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setPadding(0, 12, 0, 0);
        row->setMarginBottom(3);
        row->setCornerRadius(6);
        row->setHighlightCornerRadius(6);
        row->setHighlightPadding(0);
        row->setFocusable(true);

        auto* marker = new brls::Rectangle();
        marker->setWidth(3);
        marker->setHeight(20);
        marker->setMarginRight(12);
        marker->setColor(nvgRGBA(0, 0, 0, 0));
        row->addView(marker);

        auto* text = MakeParagraph(label, 0.0f);
        text->setFontSize(16);
        text->setTextColor(nvgRGB(184, 190, 200));
        row->addView(text);

        row->registerClickAction([this, category](brls::View*) {
            SelectCategory(category);
            return true;
        });
        category_nav_items_.push_back({row, marker, text});
        sidebar->addView(row);
    }

    auto* nav_hint = MakeParagraph(
        "Stream changes apply when the next game starts.", 0.0f);
    nav_hint->setFontSize(14);
    nav_hint->setTextColor(nvgRGB(124, 132, 144));
    nav_hint->setSingleLine(false);
    sidebar->addView(nav_hint);
    body->addView(sidebar);

    auto* sidebar_divider = new brls::Rectangle();
    sidebar_divider->setWidth(1);
    sidebar_divider->setMarginRight(24);
    sidebar_divider->setColor(nvgRGBA(255, 255, 255, 16));
    body->addView(sidebar_divider);

    auto* content_shell = new brls::Box(brls::Axis::COLUMN);
    content_shell->setGrow(1.0f);
    content_shell->setPadding(0, 0, 0, 0);

    page_title_ = MakeParagraph("Account", 2.0f);
    page_title_->setFontSize(27);
    content_shell->addView(page_title_);
    page_subtitle_ = MakeParagraph("Manage your GeForce NOW identity and saved sign-in.", 14.0f);
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

    SelectCategory(Category::Account);
}

brls::Box* SettingsTab::MakeSection(const std::string& title, const std::string& subtitle)
{
    auto* section = new brls::Box(brls::Axis::COLUMN);
    section->setPadding(16, 18, 16, 18);
    section->setMarginBottom(14);
    section->setCornerRadius(6);
    section->setBorderThickness(1);
    section->setBorderColor(nvgRGB(42, 46, 54));
    section->setBackgroundColor(nvgRGB(18, 20, 24));

    auto* label = MakeParagraph(title, subtitle.empty() ? 14.0f : 4.0f);
    label->setFontSize(19);
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
    row->setHeight(64);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setPadding(7, 8, 7, 8);
    row->setMarginBottom(4);

    auto* copy = new brls::Box(brls::Axis::COLUMN);
    copy->setGrow(1.0f);
    copy->setMarginRight(18);
    auto* name = MakeParagraph(title, 2.0f);
    name->setFontSize(18);
    copy->addView(name);
    auto* detail = MakeParagraph(description, 0.0f);
    detail->setFontSize(13);
    detail->setTextColor(nvgRGB(132, 140, 151));
    copy->addView(detail);
    row->addView(copy);

    auto* button = new brls::Button();
    button->setWidth(224);
    button->setHeight(42);
    button->setCornerRadius(6);
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
    row->setHeight(64);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setPadding(7, 8, 7, 8);
    row->setMarginBottom(4);

    auto* copy = new brls::Box(brls::Axis::COLUMN);
    copy->setGrow(1.0f);
    copy->setMarginRight(18);
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
    button->setWidth(224);
    button->setHeight(42);
    button->setCornerRadius(6);
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
            page_subtitle_->setText(Tr("Choose a simple quality profile for your connection."));
            break;
        case Category::Preferences:
            page_title_->setText(Tr("Preferences"));
            page_subtitle_->setText(Tr("Game, controller and audio choices in one place."));
            break;
        case Category::App:
            page_title_->setText(Tr("App"));
            page_subtitle_->setText(Tr("Language, local storage and app information."));
            break;
    }

    UpdateCategoryChrome();
    RebuildCategory();
}

void SettingsTab::UpdateCategoryChrome()
{
    const size_t selected = static_cast<size_t>(category_);
    for (size_t index = 0; index < category_nav_items_.size(); ++index)
    {
        const auto& item = category_nav_items_[index];
        const bool active = index == selected;
        item.row->setBackgroundColor(
            active ? nvgRGBA(77, 218, 130, 24) : nvgRGBA(0, 0, 0, 0));
        item.marker->setColor(
            active ? nvgRGB(77, 218, 130) : nvgRGBA(0, 0, 0, 0));
        item.label->setTextColor(
            active ? nvgRGB(245, 247, 249) : nvgRGB(184, 190, 200));
    }
}

void SettingsTab::RebuildCategory()
{
    if (!content_container_)
        return;

    const size_t selected = static_cast<size_t>(category_);
    brls::View* stable_focus = selected < category_nav_items_.size()
        ? category_nav_items_[selected].row
        : static_cast<brls::View*>(this);
    MoveFocusBeforeDestroy(content_container_, stable_focus);

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
        case Category::Preferences:
            BuildPreferencesPage();
            break;
        case Category::App:
            BuildAppPage();
            break;
    }
    if (scrolling_frame_)
        scrolling_frame_->setContentOffsetY(0.0f, false);
    UpdateOptionValues();
}

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
        AddInfoLine(
            overview, "Membership",
            membership::DisplayLabel(
                session.user.membership_tier, session.user.membership_tier_verified));
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
        "Connect with NVIDIA using a QR code.",
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

    const ImageCacheInfo images = ReadImageCacheInfo();
    auto* cache = MakeSection("Storage", "Cover artwork is cached to keep the library responsive.");
    AddInfoLine(cache, "Cached covers", std::to_string(images.files));
    AddInfoLine(cache, "Disk usage", FormatBytes(images.bytes));
    cache->addView(MakeActionRow(
        "Clear cover artwork", "Covers will download again when needed.", "Clear",
        [this](brls::View* view) { return ClearCoverCache(view); }, true));
    content_container_->addView(cache);

    auto* about = MakeSection("OpenNOW", "Native GeForce NOW client for Nintendo Switch.");
    AddInfoLine(about, "Version", "1.0.0");
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

    const bool interface_language_changed =
        draft_settings_.interface_language != saved_settings_.interface_language;
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
    const std::array<const char*, 4> category_names {
        "Account", "Stream", "Preferences", "App"};
    for (size_t index = 0; index < category_nav_items_.size() && index < category_names.size(); ++index)
        category_nav_items_[index].label->setText(Tr(category_names[index]));
    if (interface_language_changed)
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

bool SettingsTab::BeginLogin(brls::View* view)
{
    (void)view;
    try
    {
        auto& state = AppState::Instance();
        std::vector<LoginProvider> providers =
            state.HasProviders() ? state.providers() : client_.FetchLoginProviders();
        if (!state.HasProviders())
            state.SetProviders(providers);
        if (providers.empty())
            throw std::runtime_error("No GeForce NOW login providers were returned");

        const LoginProvider provider =
            state.HasSession() && state.session()->reauthentication_required
            ? state.session()->provider
            : providers.front();
        auto* dialog = new QrLoginDialog(provider, client_, [this]() {
            RefreshSummary();
            RebuildCategory();
        });
        brls::Application::pushActivity(new brls::Activity(dialog));
    }
    catch (const std::exception& ex)
    {
        ShowError("GeForce NOW Login Failed", ex.what());
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

            AuthSession refresh_source = session;
            refresh_source.membership_checked_at_ms = 0;
            GfnClient client = client_;
            brls::async([this, client, refresh_source = std::move(refresh_source)]() mutable {
                try
                {
                    AuthSession refreshed = client.EnsureFreshSavedSession(refresh_source);
                    brls::sync([this, refreshed = std::move(refreshed)]() mutable {
                        auto& current = AppState::Instance();
                        if (!current.HasSession() ||
                            current.session()->user.user_id != refreshed.user.user_id)
                            return;
                        current.SetSession(std::move(refreshed));
                        RebuildCategory();
                    });
                }
                catch (const std::exception&)
                {
                    // Keep the saved account active when subscription refresh is offline.
                }
            }, false);
        });
    }
    dialog->addButton("Cancel", [] {});
    dialog->open();
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

bool SettingsTab::CycleVideoBackend(brls::View* view)
{
    (void)view;
    settings::CycleVideoBackend(draft_settings_);
    MarkDirty();
    return true;
}

std::string SettingsTab::ServerLocationValue() const
{
    if (server_locations_loading_)
        return "Testing locations...";

    const auto best = std::find_if(
        server_locations_.begin(), server_locations_.end(),
        [](const StreamRegion& region) { return region.ping_ms >= 0; });

    if (server_location::IsAutomatic(draft_settings_.region))
    {
        if (best == server_locations_.end())
            return "Auto (best)";
        return "Auto · " + best->name + " · " +
            std::to_string(best->ping_ms) + " ms";
    }

    const auto selected = std::find_if(
        server_locations_.begin(), server_locations_.end(),
        [this](const StreamRegion& region) {
            return region.url == draft_settings_.region;
        });
    if (selected == server_locations_.end())
        return "Selected server";

    std::string value = selected->name;
    if (selected->ping_ms >= 0)
        value += " · " + std::to_string(selected->ping_ms) + " ms";
    return value;
}

void SettingsTab::BeginServerLocationLoad(
    bool open_when_ready,
    bool notify_result)
{
    if (server_locations_loading_)
    {
        open_server_location_when_ready_ =
            open_server_location_when_ready_ || open_when_ready;
        if (notify_result)
            brls::Application::notify("Server location test is already running");
        return;
    }

    EnsureSessionLoaded();
    auto& state = AppState::Instance();
    if (!state.HasSession())
    {
        if (open_when_ready || notify_result)
        {
            ShowError(
                "Server Locations Unavailable",
                "Connect a GeForce NOW account before loading server locations.");
        }
        return;
    }

    server_locations_loading_ = true;
    server_locations_error_.clear();
    open_server_location_when_ready_ = open_when_ready;
    UpdateOptionValues();

    GfnClient client = client_;
    AuthSession session = *state.session();
    brls::async(
        [this, client, session = std::move(session), notify_result]() mutable {
            try
            {
                std::vector<StreamRegion> regions =
                    client.FetchStreamRegions(session);
                regions = client.MeasureStreamRegionLatencies(std::move(regions));
                std::stable_sort(
                    regions.begin(), regions.end(),
                    [](const StreamRegion& left, const StreamRegion& right) {
                        const bool left_ok = left.ping_ms >= 0;
                        const bool right_ok = right.ping_ms >= 0;
                        if (left_ok != right_ok)
                            return left_ok;
                        if (left_ok && left.ping_ms != right.ping_ms)
                            return left.ping_ms < right.ping_ms;
                        return left.name < right.name;
                    });

                brls::sync(
                    [this, session = std::move(session),
                     regions = std::move(regions), notify_result]() mutable {
                        auto& current = AppState::Instance();
                        if (current.HasSession() &&
                            current.session()->user.user_id == session.user.user_id)
                        {
                            current.SetSession(std::move(session));
                        }

                        server_locations_ = std::move(regions);
                        server_locations_loaded_ = true;
                        server_locations_loading_ = false;
                        if (server_locations_.empty())
                        {
                            server_locations_error_ =
                                "GeForce NOW did not return any server locations.";
                        }
                        UpdateOptionValues();

                        const bool should_open =
                            open_server_location_when_ready_ &&
                            category_ == Category::Stream;
                        open_server_location_when_ready_ = false;
                        if (should_open)
                        {
                            ChooseServerLocation(nullptr);
                        }
                        else if (notify_result)
                        {
                            brls::Application::notify(
                                server_locations_.empty()
                                    ? server_locations_error_
                                    : "Server location latency test complete");
                        }
                    });
            }
            catch (const std::exception& ex)
            {
                const std::string message = ex.what();
                brls::sync([this, message, notify_result] {
                    const bool should_open =
                        open_server_location_when_ready_ &&
                        category_ == Category::Stream;
                    server_locations_loading_ = false;
                    server_locations_loaded_ =
                        !server_locations_.empty() || should_open;
                    server_locations_error_ = message;
                    open_server_location_when_ready_ = false;
                    UpdateOptionValues();
                    if (should_open)
                    {
                        brls::Application::notify(
                            "Latency test failed; automatic routing is still available");
                        ChooseServerLocation(nullptr);
                    }
                    else if (notify_result)
                    {
                        ShowError("Server Location Test Failed", message);
                    }
                });
            }
        },
        false);
}

bool SettingsTab::ChooseServerLocation(brls::View* view)
{
    (void)view;
    if (server_locations_loading_)
    {
        open_server_location_when_ready_ = true;
        brls::Application::notify("Testing GeForce NOW server locations...");
        return true;
    }

    if (!server_locations_loaded_)
    {
        BeginServerLocationLoad(true, false);
        return true;
    }

    const auto best = std::find_if(
        server_locations_.begin(), server_locations_.end(),
        [](const StreamRegion& region) { return region.ping_ms >= 0; });

    std::vector<StreamRegion> options = server_locations_;
    if (!server_location::IsAutomatic(draft_settings_.region))
    {
        const bool selected_available = std::any_of(
            options.begin(), options.end(), [this](const StreamRegion& region) {
                return region.url == draft_settings_.region;
            });
        if (!selected_available)
        {
            options.push_back(
                {"Current server (not advertised)", draft_settings_.region, -1});
        }
    }

    std::vector<std::string> labels;
    labels.reserve(options.size() + 1);
    std::string automatic = "Auto (best)";
    if (best != server_locations_.end())
    {
        automatic += " — " + best->name + " · " +
            std::to_string(best->ping_ms) + " ms";
    }
    labels.push_back(std::move(automatic));

    int selected_index = 0;
    for (size_t index = 0; index < options.size(); ++index)
    {
        const StreamRegion& region = options[index];
        std::string label = region.name;
        if (region.ping_ms >= 0)
            label += " — " + std::to_string(region.ping_ms) + " ms";
        else
            label += " — Unavailable";
        if (best != server_locations_.end() && region.url == best->url)
            label += " · Best";
        labels.push_back(std::move(label));

        if (region.url == draft_settings_.region)
            selected_index = static_cast<int>(index + 1);
    }

    auto* dropdown = new brls::Dropdown(
        "Server location", labels,
        [this, options = std::move(options)](int index) {
            if (index < 0)
                return;
            draft_settings_.region =
                index == 0 || static_cast<size_t>(index) > options.size()
                ? "Auto"
                : options[static_cast<size_t>(index - 1)].url;
            MarkDirty();
            UpdateOptionValues();
        },
        selected_index);
    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}

bool SettingsTab::RefreshServerLocations(brls::View* view)
{
    (void)view;
    BeginServerLocationLoad(false, true);
    return true;
}

bool SettingsTab::ToggleCommunityProxy(brls::View* view)
{
    (void)view;
    if (community_proxy_provisioning_)
        return true;

    if (draft_settings_.community_proxy_enabled)
    {
        draft_settings_.community_proxy_enabled = false;
        MarkDirty();
        return true;
    }

    auto* dialog = new brls::Dialog(
        "The Zortos community proxy is optional, shared and may be rate-limited or "
        "unavailable. It only routes NVIDIA catalog and session requests; streaming "
        "traffic stays direct.");
    dialog->addButton("Enable proxy", [this]() {
        community_proxy_provisioning_ = true;
        UpdateOptionValues();
        brls::Application::notify("Activating community proxy...");

        GfnClient client = client_;
        brls::async([this, client]() mutable {
            try
            {
                std::string proxy_url = client.ProvisionCommunityProxy();
                brls::sync([this, proxy_url = std::move(proxy_url)]() mutable {
                    draft_settings_.community_proxy_url = std::move(proxy_url);
                    draft_settings_.community_proxy_enabled = true;
                    community_proxy_provisioning_ = false;
                    MarkDirty();
                    UpdateOptionValues();
                    brls::Application::notify(
                        "Community proxy ready; press X to save");
                });
            }
            catch (const std::exception& ex)
            {
                const std::string message = ex.what();
                brls::sync([this, message] {
                    community_proxy_provisioning_ = false;
                    UpdateOptionValues();
                    ShowError("Community Proxy Failed", message);
                });
            }
        }, false);
    });
    dialog->addButton("Cancel", [] {});
    dialog->open();
    return true;
}

bool SettingsTab::CycleImageQuality(brls::View* view)
{
    (void)view;
    settings::CycleImageQuality(draft_settings_);
    MarkDirty();
    return true;
}

bool SettingsTab::ToggleStatsOverlay(brls::View* view)
{
    (void)view;
    draft_settings_.stats_overlay_enabled = !draft_settings_.stats_overlay_enabled;
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

bool SettingsTab::ShowHomeScreenHelp(brls::View* view)
{
    (void)view;
    auto* dialog = new brls::Dialog(
        "OpenNOW will generate a Horizon forwarder and install it to SD "
        "storage. The HOME icon starts the existing OpenNOW NRO, so future "
        "OpenNOW updates do not require reinstalling the forwarder.\n\n"
        "Continue?");
    dialog->addButton("Install on HOME", [] {
        std::string error;
        if (!shortcut::StartForwarderInstaller(
                shortcut::ExecutablePath(), "OpenNOW", error))
            ShowError("HOME Install Failed", error);
    });
    dialog->addButton("Cancel", [] {});
    dialog->setCancelable(true);
    dialog->open();
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

} // namespace opennow
