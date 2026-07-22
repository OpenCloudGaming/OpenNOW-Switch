#include "settings_tab.hpp"

#include "app_state.hpp"
#include "app_paths.hpp"
#include "localization.hpp"
#include "membership_tier_policy.hpp"
#include "stream_settings.hpp"
#include "stream_settings_policy.hpp"
#include "stream_diagnostics.hpp"
#include "ui_helpers.hpp"

#include <cstdint>
#include <algorithm>
#include <array>
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
    setPadding(18, 32, 18, 32);

    auto* top = new brls::Box(brls::Axis::ROW);
    top->setHeight(68);
    top->setAlignItems(brls::AlignItems::CENTER);
    top->setMarginBottom(12);

    auto* title_column = new brls::Box(brls::Axis::COLUMN);
    title_column->setGrow(1.0f);
    auto* title = MakeParagraph("Settings", 3.0f);
    title->setFontSize(30);
    title_column->addView(title);
    auto* hint = MakeParagraph("Choose what matters, then press X to save.", 0.0f);
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
    sidebar->setWidth(224);
    sidebar->setPadding(16, 14, 16, 14);
    sidebar->setMarginRight(24);
    sidebar->setCornerRadius(14);
    sidebar->setBackgroundColor(nvgRGB(15, 17, 21));

    auto* nav_label = MakeParagraph("Settings", 12.0f);
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
        auto* button = new brls::Button();
        button->setText(Tr(label));
        button->setHeight(54);
        button->setMarginBottom(8);
        button->setCornerRadius(10);
        button->registerClickAction([this, category](brls::View*) {
            SelectCategory(category);
            return true;
        });
        category_buttons_.push_back(button);
        sidebar->addView(button);
    }

    auto* nav_hint = MakeParagraph(
        "Stream changes apply when the next game starts.", 0.0f);
    nav_hint->setFontSize(14);
    nav_hint->setTextColor(nvgRGB(124, 132, 144));
    nav_hint->setSingleLine(false);
    sidebar->addView(nav_hint);
    body->addView(sidebar);

    auto* content_shell = new brls::Box(brls::Axis::COLUMN);
    content_shell->setGrow(1.0f);
    content_shell->setPadding(0, 0, 0, 0);

    page_title_ = MakeParagraph("Stream", 2.0f);
    page_title_->setFontSize(27);
    content_shell->addView(page_title_);
    page_subtitle_ = MakeParagraph("Pick a quality profile that fits your connection.", 14.0f);
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
    section->setPadding(18, 20, 18, 20);
    section->setMarginBottom(16);
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
    row->setHeight(68);
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
    button->setHeight(48);
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
    row->setHeight(68);
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
    button->setHeight(48);
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
    auto* overview = MakeSection("Connected account", "OpenNOW keeps your sign-in ready between launches.");
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
    }
    AddInfoLine(overview, "Saved accounts", std::to_string(client_.LoadSavedSessions().size()));
    content_container_->addView(overview);

    auto* session = MakeSection("Account actions", "These actions take effect immediately.");
    session->addView(MakeActionRow(
        "Choose saved account", "Switch profiles without repeating sign-in.", "Choose",
        [this](brls::View* view) { return SwitchSavedAccount(view); }));
    session->addView(MakeActionRow(
        "Remove active account", "Disconnect this account from the console.", "Remove",
        [this](brls::View* view) { return ClearSavedLogin(view); }, true));
    content_container_->addView(session);
}

void SettingsTab::BuildStreamPage()
{
    auto* quality = MakeSection(
        "Stream quality",
        "Each profile sets resolution, frame rate and bitrate together.");
    quality->addView(MakeOptionRow(
        "Quality profile", "Safe favors stability; Quality favors detail.",
        [this] {
            return draft_settings_.label + " / " +
                std::to_string(draft_settings_.height) + "p / " +
                std::to_string(draft_settings_.fps);
        },
        [this](brls::View* view) { return CycleStreamPreset(view); }));
    quality->addView(MakeOptionRow(
        "Picture processing",
        "Adaptive is recommended; Clarity sharpens motion; Original keeps the source unchanged.",
        [this] { return draft_settings_.image_quality_mode; },
        [this](brls::View* view) { return CycleImageQuality(view); }));
    AddInfoLine(quality, "Decoder", "Automatic hardware decode with fallback");
    AddInfoLine(quality, "Server", "Automatic selection for best latency");
    content_container_->addView(quality);
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

} // namespace opennow
