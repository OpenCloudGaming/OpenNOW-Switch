#include "ui_theme.hpp"

#include "localization.hpp"

#include <borealis/core/assets.hpp>
#include <borealis/core/font.hpp>
#include <borealis/core/touch/tap_gesture.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace opennow::ui
{
namespace
{

constexpr std::array<const char*, 5> kFontNames {
    "regular", "opennow-semibold", "opennow-bold", "opennow-extrabold", "opennow-mono"};

}

void InitializeThemeAndFonts()
{
    const int original = brls::Application::getFont(brls::FONT_REGULAR);
    const std::array<const char*, 5> paths {
        "font/Nunito-Medium.ttf", "font/Nunito-SemiBold.ttf", "font/Nunito-Bold.ttf",
        "font/Nunito-ExtraBold.ttf", "font/IBMPlexMono-Medium.ttf"};
    for (size_t index = 0; index < paths.size(); ++index)
    {
        if (!brls::Application::loadFontFromFile(kFontNames[index], std::string(BRLS_ASSET("")) + paths[index]))
            throw std::runtime_error("Cannot load bundled OpenNOW font: " + std::string(paths[index]));
    }
    if (!brls::Application::loadFontFromFile("opennow-cjk", BRLS_ASSET("font/OpenNOW-CJK.ttf")))
        throw std::runtime_error("Cannot load bundled OpenNOW Chinese fallback font");

    std::vector<int> fallbacks {brls::Application::getFont("opennow-cjk")};
    for (const auto& name : {brls::FONT_CHINESE_SIMPLIFIED, brls::FONT_CHINESE_SIMPLIFIED_EXT,
                            brls::FONT_CHINESE_TRADITIONAL, brls::FONT_KOREAN_REGULAR,
                            brls::FONT_SWITCH_ICONS, brls::FONT_MATERIAL_ICONS, brls::FONT_EMOJI})
        fallbacks.push_back(brls::Application::getFont(name));
    fallbacks.push_back(original);

    for (const char* name : kFontNames)
    {
        const int base = brls::Application::getFont(name);
        for (int fallback : fallbacks)
            if (fallback != brls::FONT_INVALID && fallback != base)
                nvgAddFallbackFontId(brls::Application::getNVGContext(), base, fallback);
    }

    for (brls::Theme* theme : {&brls::Theme::getDarkTheme(), &brls::Theme::getLightTheme()})
    {
        theme->addColor("brls/background", Ground());
        theme->addColor("brls/text", Text());
        theme->addColor("brls/text_disabled", Muted());
        theme->addColor("brls/accent", Green());
        theme->addColor("brls/highlight/color1", Green());
        theme->addColor("brls/highlight/color2", Green());
        theme->addColor("brls/highlight/background", Raised());
        theme->addColor("brls/click_pulse", nvgRGBA(118, 232, 58, 36));
        theme->addColor("brls/button/primary_enabled_background", Green());
        theme->addColor("brls/button/primary_enabled_text", Ground());
        theme->addColor("brls/button/default_enabled_background", Raised());
        theme->addColor("brls/button/default_enabled_text", Text());
        theme->addColor("brls/button/highlight_enabled_text", Green());
        theme->addColor("brls/button/enabled_border_color", Rule());
        theme->addColor("brls/list/listItem_value_color", Green());
        theme->addColor("brls/sidebar/background", Ground());
        theme->addColor("brls/sidebar/active_item", Green());
        theme->addColor("brls/header/subtitle", Muted());
    }
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::getStyle().addMetric("brls/highlight/stroke_width", 2.0f);
}

int Font(FontRole role)
{
    const int handle = brls::Application::getFont(kFontNames[static_cast<size_t>(role)]);
    return handle == brls::FONT_INVALID ? brls::Application::getDefaultFont() : handle;
}

StyledLabel::StyledLabel(FontRole role)
{
    font = Font(role);
    setAutoAnimate(false);
}

brls::Label* MakeLabel(const std::string& text, float size, NVGcolor color, FontRole role)
{
    auto* label = new StyledLabel(role);
    label->setText(text);
    label->setFontSize(size);
    label->setTextColor(color);
    label->setLineHeight(1.35f);
    return label;
}

float TextWidth(const std::string& text, float size, FontRole role)
{
    auto* vg = brls::Application::getNVGContext();
    nvgSave(vg);
    nvgFontFaceId(vg, Font(role));
    nvgFontSize(vg, size);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    float bounds[4] {};
    const float advance = nvgTextBounds(vg, 0, 0, text.c_str(), nullptr, bounds);
    nvgRestore(vg);
    return std::ceil(std::max(advance, bounds[2]) - std::min(0.0f, bounds[0]));
}

void FadingScrollFrame::draw(NVGcontext* vg, float x, float y, float width, float height,
                           brls::Style style, brls::FrameContext* ctx)
{
    brls::ScrollingFrame::draw(vg, x, y, width, height, style, ctx);
    if (getContentHeight() - getContentOffsetY() <= height + 0.5f)
        return;
    nvgSave(vg);
    nvgBeginPath(vg);
    nvgRect(vg, x, y + height - 40, width, 40);
    const auto ground = Ground();
    auto transparent = ground;
    transparent.a = 0;
    nvgFillPaint(vg, nvgLinearGradient(vg, x, y + height - 40, x, y + height,
        transparent, ground));
    nvgFill(vg);
    nvgRestore(vg);
}

ActionRow::ActionRow(std::string title, std::string value,
                     std::function<bool(brls::View*)> activate, ActionTone tone)
    : brls::Box(brls::Axis::ROW), tone_(tone)
{
    setHeight(56);
    setShrink(0);
    setPadding(0, 18, 0, 18);
    setAlignItems(brls::AlignItems::CENTER);
    setCornerRadius(12);
    setBorderThickness(1);
    setFocusable(true);
    setHideHighlightBackground(true);
    setHighlightPadding(3);
    setHighlightCornerRadius(14);
    title_ = MakeLabel(title, 18, Text(), FontRole::Medium);
    title_->setGrow(1);
    title_->setShrink(1);
    title_->setMinWidth(0);
    title_->setSingleLine(true);
    addView(title_);
    value_ = MakeLabel(value, 16, Muted());
    value_->setMaxWidthPercentage(45);
    value_->setShrink(1);
    value_->setMarginLeft(16);
    value_->setSingleLine(true);
    value_->setVisibility(value.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    addView(value_);
    registerClickAction(std::move(activate));
    addGestureRecognizer(new brls::TapGestureRecognizer(this));
    updateActionHint(brls::BUTTON_A, title);
    UpdateChrome(false);
}

void ActionRow::SetTitle(const std::string& title)
{
    title_->setText(title);
    updateActionHint(brls::BUTTON_A, title);
}
void ActionRow::SetValue(const std::string& value)
{
    value_->setText(value);
    value_->setVisibility(value.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
}
void ActionRow::SetFontSize(float size) { title_->setFontSize(size); }
void ActionRow::SetTone(ActionTone tone)
{
    tone_ = tone;
    UpdateChrome(brls::Application::getCurrentFocus() == this);
}
void ActionRow::SetFocusHandler(std::function<void()> handler) { focus_handler_ = std::move(handler); }

void ActionRow::onFocusGained()
{
    brls::Box::onFocusGained();
    UpdateChrome(true);
    if (focus_handler_)
        focus_handler_();
}

void ActionRow::onFocusLost()
{
    brls::Box::onFocusLost();
    UpdateChrome(false);
}

void ActionRow::UpdateChrome(bool focused)
{
    const bool primary = tone_ == ActionTone::Primary;
    const bool flat = tone_ == ActionTone::Flat;
    setBackgroundColor(primary ? Green() : focused ? nvgRGB(30, 32, 36) : flat ? Ground() : Raised());
    setBorderColor(focused ? Green() : flat ? nvgRGBA(0, 0, 0, 0) : Rule());
    title_->setTextColor(primary ? Ground() : tone_ == ActionTone::Destructive ? Danger() : Text());
    value_->setTextColor(primary ? Ground() : focused ? Green() : Muted());
}

std::string StreamSummary(const StreamSettings& settings)
{
    std::string bitrate = std::to_string(settings.bitrate_kbps / 1000);
    if (const int remainder = settings.bitrate_kbps % 1000; remainder != 0)
    {
        std::string fraction = std::to_string(1000 + remainder).substr(1);
        fraction.erase(fraction.find_last_not_of('0') + 1);
        bitrate += "." + fraction;
    }
    return std::to_string(settings.width) + "×" + std::to_string(settings.height) + " · " +
        std::to_string(settings.fps) + " FPS · " + bitrate + " Mbps";
}

std::string ConfiguredLocation(const std::string& region)
{
    return region.empty() || region == "auto" || region == "Auto"
        ? Tr("Auto") : Tr("Selected server");
}

NextStreamSummaryView::NextStreamSummaryView(const StreamSettings& settings)
    : brls::Box(brls::Axis::COLUMN)
{
    setPadding(14, 18, 14, 18);
    setCornerRadius(12);
    setBorderThickness(1);
    setBorderColor(Rule());
    setBackgroundColor(Raised());
    heading_ = MakeLabel(Tr("Next stream"), 14, Muted());
    heading_->setMarginBottom(8);
    addView(heading_);
    summary_ = MakeLabel({}, 18, Text(), FontRole::Medium);
    summary_->setSingleLine(true);
    addView(summary_);
    detail_ = MakeLabel({}, 14, Muted());
    detail_->setMarginTop(5);
    detail_->setSingleLine(true);
    addView(detail_);
    Update(settings);
}

void NextStreamSummaryView::Update(const StreamSettings& settings)
{
    heading_->setText(Tr("Next stream"));
    summary_->setText(StreamSummary(settings));
    detail_->setText(Tr("Configured") + " · " + settings.codec + " · " +
                    ConfiguredLocation(settings.region));
}

}
