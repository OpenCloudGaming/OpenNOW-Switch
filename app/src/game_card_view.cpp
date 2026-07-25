#include "game_card_view.hpp"

#include "cover_image_cache.hpp"

#include <utility>

namespace opennow
{
namespace
{

std::string BuildSubtitle(const GameCardDisplay& display)
{
    if (display.badge.empty())
        return display.subtitle;

    if (display.subtitle.empty())
        return display.badge;

    return display.subtitle + "  |  " + display.badge;
}

brls::Label* MakeLabel(const std::string& text, float size, NVGcolor color)
{
    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(size);
    label->setTextColor(color);
    label->setSingleLine(true);
    return label;
}

} // namespace

GameCardView::GameCardView(GameCardDisplay display, ClickHandler click_handler)
    : brls::Box(brls::Axis::COLUMN)
    , display_(std::move(display))
    , click_handler_(std::move(click_handler))
{
    setWidth(224);
    setHeight(178);
    setPadding(6, 6, 7, 6);
    setMarginRight(10);
    setMarginBottom(10);
    setCornerRadius(10);
    setBorderThickness(2);
    setShadowType(brls::ShadowType::GENERIC);
    setFocusable(true);
    // Keep the card surface dark, but retain Borealis' animated focus outline.
    // The branded border color alone is too subtle at TV viewing distance.
    setHideHighlightBackground(true);
    setHighlightPadding(2);
    setHighlightCornerRadius(12);

    image_ = new brls::Image();
    image_->setWidth(212);
    image_->setHeight(112);
    image_->setCornerRadius(7);
    image_->setMarginBottom(6);
    image_->setScalingType(brls::ImageScalingType::FILL);
    addView(image_);

    title_label_ = MakeLabel(display_.title, 16.0f, nvgRGB(245, 246, 248));
    title_label_->setMarginBottom(3);
    addView(title_label_);

    subtitle_label_ = MakeLabel(BuildSubtitle(display_), 12.0f, nvgRGB(88, 217, 138));
    addView(subtitle_label_);

    UpdateChrome(false);
    LoadImage();

    registerClickAction([this](brls::View* view) {
        (void)view;
        if (click_handler_)
            click_handler_();

        return true;
    });
}

void GameCardView::onFocusGained()
{
    brls::Box::onFocusGained();
    UpdateChrome(true);
}

void GameCardView::onFocusLost()
{
    brls::Box::onFocusLost();
    UpdateChrome(false);
}

void GameCardView::UpdateChrome(bool focused)
{
    setBackgroundColor(focused ? nvgRGB(26, 42, 34) : nvgRGB(21, 21, 24));
    setBorderColor(focused ? nvgRGB(88, 217, 138) : nvgRGB(42, 42, 48));
}

void GameCardView::LoadImage()
{
    SetCachedCoverImage(image_, display_.image_url);
}

} // namespace opennow
