#include "settings_tab.hpp"

#include "app_state.hpp"
#include "localization.hpp"
#include "stream_settings.hpp"
#include "stream_diagnostics.hpp"
#include "ui_action_guard.hpp"
#include "ui_helpers.hpp"

#include <array>
#include <iomanip>
#include <sstream>
#include <utility>

namespace opennow
{

brls::Label* SettingsTab::MakeParagraph(
    const std::string& text, float bottom_margin)
{
    auto* label = new brls::Label();
    label->setText(Tr(text));
    label->setFontSize(18);
    label->setMarginBottom(bottom_margin);
    return label;
}

std::string SettingsTab::FormatBytes(std::uint64_t bytes)
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

namespace
{

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

    auto* page_heading = new brls::Box(brls::Axis::ROW);
    page_heading->setAlignItems(brls::AlignItems::CENTER);
    page_heading->setMarginBottom(2);
    page_title_ = MakeParagraph("Account", 0.0f);
    page_title_->setFontSize(27);
    page_title_->setGrow(1.0f);
    page_heading->addView(page_title_);
    save_status_ = MakeParagraph("All changes saved", 0.0f);
    save_status_->setFontSize(14);
    save_status_->setTextColor(nvgRGB(88, 230, 146));
    page_heading->addView(save_status_);
    content_shell->addView(page_heading);
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

brls::Label* SettingsTab::AddInfoLine(
    brls::Box* parent, const std::string& label, const std::string& value)
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
    return text;
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

} // namespace opennow
