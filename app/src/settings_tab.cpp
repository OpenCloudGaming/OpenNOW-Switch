#include "settings_tab.hpp"

#include "app_state.hpp"
#include "localization.hpp"
#include "stream_settings.hpp"
#include "stream_settings_policy.hpp"
#include "stream_diagnostics.hpp"
#include "ui_action_guard.hpp"
#include "ui_helpers.hpp"

#include <array>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <utility>

namespace opennow
{

brls::Label* SettingsTab::MakeParagraph(
    const std::string& text, float bottom_margin)
{
    auto* label = ui::MakeLabel(Tr(text));
    label->setSingleLine(false);
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

std::string SettingsTab::BitrateValue(int kbps)
{
    std::ostringstream value;
    value << std::fixed << std::setprecision(3) << static_cast<double>(kbps) / 1000.0;
    std::string text = value.str();
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text + " Mbps";
}

namespace
{

std::string OptionId(const std::string& title)
{
    std::string id = "settings/option/";
    for (unsigned char character : title)
        id += std::isalnum(character) ? static_cast<char>(std::tolower(character)) : '-';
    return id;
}

} // namespace

SettingsTab::SettingsTab()
    : brls::Box(brls::Axis::COLUMN)
{
    setId("settings");
    setPadding(24, 48, 20, 48);
    setBackgroundColor(ui::Ground());

    auto* body = new brls::Box(brls::Axis::ROW);
    body->setGrow(1.0f);
    body->setMinHeight(0);

    auto* sidebar = new brls::Box(brls::Axis::COLUMN);
    sidebar->setWidth(220);
    sidebar->setShrink(0);
    sidebar->setMarginRight(32);

    auto* nav_label = MakeParagraph("Settings", 20.0f);
    nav_label->setFontSize(13);
    nav_label->setTextColor(ui::Muted());
    sidebar->addView(nav_label);

    const std::array<std::pair<const char*, Category>, 4> categories {{
        {"Account", Category::Account},
        {"Stream", Category::Stream},
        {"Preferences", Category::Preferences},
        {"App", Category::App},
    }};
    for (const auto& [label, category] : categories)
    {
        auto* row = new ui::ActionRow(Tr(label), {}, [this, category](brls::View*) {
            SelectCategory(category);
            return true;
        }, ui::ActionTone::Flat);
        std::string category_id = label;
        for (char& character : category_id)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        row->setId("settings/category/" + category_id);
        row->setHeight(48);
        row->setMarginBottom(6);
        row->SetFocusHandler([this] {
            ShowOptionHelp({}, page_subtitle_->getFullText());
        });
        category_nav_items_.push_back({row, row});
        sidebar->addView(row);
    }

    save_status_ = ui::MakeLabel({}, 15, ui::Green(), ui::FontRole::Medium);
    save_status_->setId("settings/save-status");
    save_status_->setSingleLine(false);
    save_status_->setMarginTop(32);
    save_status_->setMarginBottom(8);
    sidebar->addView(save_status_);
    auto* nav_hint = MakeParagraph(
        "Stream changes apply when the next game starts.", 0.0f);
    nav_hint->setFontSize(14);
    nav_hint->setTextColor(ui::Muted());
    nav_hint->setSingleLine(false);
    sidebar->addView(nav_hint);
    body->addView(sidebar);

    auto* content_shell = new brls::Box(brls::Axis::COLUMN);
    content_shell->setWidth(560);
    content_shell->setShrink(0);
    content_shell->setMinHeight(0);
    content_shell->setMarginRight(40);

    auto* page_heading = new brls::Box(brls::Axis::ROW);
    page_heading->setAlignItems(brls::AlignItems::CENTER);
    page_heading->setMarginBottom(2);
    page_title_ = ui::MakeLabel(Tr("Account"), 28, ui::Text(), ui::FontRole::Heading);
    page_title_->setSingleLine(true);
    page_title_->setGrow(1.0f);
    page_heading->addView(page_title_);
    content_shell->addView(page_heading);
    page_subtitle_ = MakeParagraph("Manage your GeForce NOW identity and saved sign-in.", 14.0f);
    page_subtitle_->setFontSize(15);
    page_subtitle_->setTextColor(ui::Muted());
    page_subtitle_->setSingleLine(true);
    content_shell->addView(page_subtitle_);

    scrolling_frame_ = new brls::ScrollingFrame();
    scrolling_frame_->setGrow(1.0f);
    scrolling_frame_->setMinHeight(0);
    scrolling_frame_->setId("settings/options");
    scrolling_frame_->setScrollingIndicatorVisible(false);
    scrolling_frame_->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    content_container_ = new brls::Box(brls::Axis::COLUMN);
    content_container_->setPadding(4, 4, 12, 4);
    scrolling_frame_->setContentView(content_container_);
    content_shell->addView(scrolling_frame_);
    body->addView(content_shell);

    auto* help_scroll = new brls::ScrollingFrame();
    help_scrolling_frame_ = help_scroll;
    help_scroll->setWidth(332);
    help_scroll->setShrink(0);
    help_scroll->setMinHeight(0);
    help_scroll->setScrollingIndicatorVisible(false);
    auto* help = new brls::Box(brls::Axis::COLUMN);
    help->setId("settings/help");
    help->setPadding(4, 0, 12, 0);
    help_title_ = ui::MakeLabel({}, 15, ui::Muted(), ui::FontRole::Medium);
    help_title_->setId("settings/help/title");
    help_title_->setSingleLine(false);
    help_title_->setMarginBottom(14);
    help->addView(help_title_);
    help_value_ = ui::MakeLabel({}, 28, ui::Text(), ui::FontRole::Heading);
    help_value_->setId("settings/help/value");
    help_value_->setSingleLine(false);
    help_value_->setMarginBottom(16);
    help->addView(help_value_);

    bitrate_meter_ = new brls::Box(brls::Axis::ROW);
    bitrate_meter_->setHeight(42);
    bitrate_meter_->setMarginBottom(16);
    for (int step : {8, 12, 16, 20, 25})
    {
        auto* column = new brls::Box(brls::Axis::COLUMN);
        column->setGrow(1);
        column->setMarginRight(step == 25 ? 0 : 6);
        auto* bar = new brls::Rectangle();
        bar->setHeight(8);
        bar->setCornerRadius(3);
        bar->setMarginBottom(7);
        column->addView(bar);
        auto* label = ui::MakeLabel(std::to_string(step), 13, ui::Muted(), ui::FontRole::Mono);
        column->addView(label);
        bitrate_meter_->addView(column);
        bitrate_steps_.push_back({bar, label});
    }
    help->addView(bitrate_meter_);
    help_description_ = ui::MakeLabel({}, 17, ui::Text());
    help_description_->setId("settings/help/description");
    help_description_->setSingleLine(false);
    help_description_->setMarginBottom(18);
    help->addView(help_description_);
    help_next_ = ui::MakeLabel({}, 14, ui::Green(), ui::FontRole::Medium);
    help_next_->setSingleLine(false);
    help_next_->setMarginBottom(24);
    help->addView(help_next_);
    draft_summary_status_ = ui::MakeLabel({}, 14, ui::Muted(), ui::FontRole::Medium);
    draft_summary_status_->setId("settings/draft-status");
    draft_summary_status_->setSingleLine(false);
    draft_summary_status_->setMarginBottom(8);
    help->addView(draft_summary_status_);
    next_stream_summary_ = new ui::NextStreamSummaryView(draft_settings_);
    next_stream_summary_->setId("settings/summary");
    help->addView(next_stream_summary_);
    help_scroll->setContentView(help);
    body->addView(help_scroll);
    addView(body);

    registerAction(Tr("Save"), brls::BUTTON_X, [this](brls::View* view) {
        return SaveChanges(view);
    }, false, true);
    registerAction(Tr("Revert"), brls::BUTTON_Y, [this](brls::View* view) {
        return RevertChanges(view);
    }, false, true);

    SelectCategory(Category::Account);
}

SettingsTab::~SettingsTab()
{
    alive_->store(false);
}

brls::Box* SettingsTab::MakeSection(const std::string& title, const std::string& subtitle)
{
    auto* section = new brls::Box(brls::Axis::COLUMN);
    section->setMarginBottom(18);

    auto* label = ui::MakeLabel(Tr(title), 14, ui::Muted(), ui::FontRole::Medium);
    label->setSingleLine(true);
    label->setMarginBottom(8);
    section->addView(label);
    (void)subtitle;
    return section;
}

brls::Box* SettingsTab::MakeOptionRow(
    const std::string& title,
    const std::string& description,
    std::function<std::string()> value,
    std::function<bool(brls::View*)> action)
{
    auto* row = new ui::ActionRow(Tr(title), Tr(value()),
        [this, action = std::move(action)](brls::View* view) {
        const bool handled = action ? action(view) : true;
        UpdateOptionValues();
        return handled;
    }, ui::ActionTone::Flat);
    row->setId(OptionId(title));
    row->updateActionHint(brls::BUTTON_A, Tr("Change"));
    row->setMarginBottom(6);
    row->SetFocusHandler([this, title, description, value] {
        ShowOptionHelp(title, description, value);
    });
    option_values_.push_back({row, std::move(value)});
    return row;
}

brls::Box* SettingsTab::MakeActionRow(
    const std::string& title,
    const std::string& description,
    const std::string& button_text,
    std::function<bool(brls::View*)> action,
    bool destructive)
{
    auto* row = new ui::ActionRow(Tr(title), Tr(button_text), std::move(action),
        destructive ? ui::ActionTone::Destructive : ui::ActionTone::Flat);
    row->setId(OptionId(title));
    row->setMarginBottom(6);
    row->SetFocusHandler([this, title, description, button_text] {
        ShowOptionHelp(title, description, [button_text] { return button_text; });
    });
    return row;
}

brls::Label* SettingsTab::AddInfoLine(
    brls::Box* parent, const std::string& label, const std::string& value)
{
    auto* row = new brls::Box(brls::Axis::ROW);
    row->setMinHeight(40);
    row->setPadding(5, 8, 5, 8);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setMarginBottom(2);
    row->setCornerRadius(7);
    auto* key = MakeParagraph(label, 0.0f);
    key->setWidthPercentage(40);
    key->setMarginRight(12);
    key->setShrink(0);
    key->setSingleLine(true);
    key->setFontSize(15);
    key->setTextColor(ui::Muted());
    row->addView(key);
    auto* text = MakeParagraph(value, 0.0f);
    text->setGrow(1);
    text->setShrink(1);
    text->setSingleLine(true);
    text->setFontSize(15);
    text->setTextColor(ui::Text());
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
            page_subtitle_->setText(Tr("Stream changes apply when the next game starts."));
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
        item.action->SetTone(active ? ui::ActionTone::Neutral : ui::ActionTone::Flat);
        item.action->SetValue(active ? "›" : "");
    }
}

void SettingsTab::RebuildCategory()
{
    if (!content_container_)
        return;

    SyncServerLocationAccount();
    const size_t selected = static_cast<size_t>(category_);
    brls::View* stable_focus = selected < category_nav_items_.size()
        ? category_nav_items_[selected].row
        : static_cast<brls::View*>(this);
    MoveFocusBeforeDestroy(content_container_, stable_focus);
    ShowOptionHelp({}, page_subtitle_->getFullText());

    option_values_.clear();
    cover_cache_files_ = nullptr;
    cover_cache_bytes_ = nullptr;
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
    for (auto& [row, value] : option_values_)
    {
        if (row && value)
            row->SetValue(Tr(value()));
    }
    UpdateOptionHelp();
}

void SettingsTab::ShowOptionHelp(
    const std::string& title, const std::string& description,
    std::function<std::string()> value)
{
    if (help_scrolling_frame_ && help_option_ != title)
        help_scrolling_frame_->setContentOffsetY(0, false);
    help_option_ = title;
    help_description_text_ = description;
    help_value_provider_ = std::move(value);
    UpdateOptionHelp();
}

void SettingsTab::UpdateOptionHelp()
{
    if (!help_title_)
        return;

    const bool bitrate = help_option_ == "Bitrate";
    help_title_->setText(help_option_.empty() ? page_title_->getFullText() : Tr(help_option_));
    help_value_->setText(help_value_provider_ ? Tr(help_value_provider_()) : "");
    help_value_->setFontSize(bitrate ? 40 : 24);
    help_value_->setVisibility(help_value_provider_ ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    help_description_->setText(Tr(help_description_text_));
    bitrate_meter_->setVisibility(bitrate ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    help_next_->setVisibility(bitrate ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    if (bitrate)
    {
        constexpr std::array<int, 5> steps {8000, 12000, 16000, 20000, 25000};
        bool matched = false;
        for (size_t index = 0; index < steps.size(); ++index)
        {
            const bool selected = steps[index] == draft_settings_.bitrate_kbps;
            matched = matched || selected;
            bitrate_steps_[index].first->setColor(selected ? ui::Green() : ui::Rule());
            bitrate_steps_[index].second->setTextColor(selected ? ui::Green() : ui::Muted());
        }
        if (!matched)
            help_title_->setText(Tr("Bitrate") + " · " + Tr("Custom"));
        auto next = draft_settings_;
        settings::CycleBitrate(next);
        help_next_->setText(Tr("Next value") + ": " + BitrateValue(next.bitrate_kbps));
    }
}

void SettingsTab::MarkDirty()
{
    dirty_ = draft_settings_ != saved_settings_;
    RefreshSummary();
}

bool SettingsTab::SaveChanges(brls::View* view)
{
    (void)view;
    if (community_proxy_provisioning_)
    {
        brls::Application::notify(Tr("Connecting..."));
        return true;
    }
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
        category_nav_items_[index].action->SetTitle(Tr(category_names[index]));
    if (interface_language_changed)
        SelectCategory(category_);
    brls::Application::notify(Tr("Settings saved; changes apply to the next stream"));
    return true;
}

bool SettingsTab::RevertChanges(brls::View* view)
{
    (void)view;
    ++proxy_request_generation_;
    community_proxy_provisioning_ = false;
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
        save_status_->setText(Tr("Unsaved changes"));
        save_status_->setTextColor(ui::Green());
    }
    else
    {
        save_status_->setText(Tr("All changes saved"));
        save_status_->setTextColor(ui::Muted());
    }
    draft_summary_status_->setText(Tr(dirty_ ? "Unsaved changes" : "All changes saved"));
    draft_summary_status_->setTextColor(dirty_ ? ui::Green() : ui::Muted());
    next_stream_summary_->Update(draft_settings_);
    UpdateOptionHelp();
}

} // namespace opennow
