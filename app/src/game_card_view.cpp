#include "game_card_view.hpp"

#include "cover_image_cache.hpp"
#include "localization.hpp"
#include "ui_theme.hpp"

#include <borealis/core/touch/tap_gesture.hpp>

#include <utility>

namespace opennow
{
GameCardView::GameCardView(GameCardDisplay display, ClickHandler click_handler)
    : brls::Box(brls::Axis::COLUMN)
    , display_(std::move(display))
    , click_handler_(std::move(click_handler))
{
    setWidth(220);
    setHeight(210);
    setShrink(0);
    setPadding(8, 8, 8, 8);
    setMarginBottom(12);
    setCornerRadius(12);
    setBorderThickness(1);
    setFocusable(true);
    setHideHighlightBackground(true);
    setHighlightPadding(3);
    setHighlightCornerRadius(14);

    image_ = new CachedImage();
    image_->setWidth(204);
    image_->setHeight(114);
    image_->setShrink(0);
    image_->setCornerRadius(7);
    image_->setMarginBottom(10);
    image_->setScalingType(brls::ImageScalingType::FILL);
    addView(image_);

    title_label_ = ui::MakeLabel(display_.title, 18, ui::Text(), ui::FontRole::Heading);
    title_label_->setId("catalog-card-title");
    title_label_->setWidthPercentage(100);
    title_label_->setHeight(26);
    title_label_->setSingleLine(true);
    addView(title_label_);

    subtitle_label_ = ui::MakeLabel(display_.subtitle, 14, ui::Muted());
    subtitle_label_->setId("catalog-card-store");
    subtitle_label_->setWidthPercentage(100);
    subtitle_label_->setHeight(20);
    subtitle_label_->setSingleLine(true);
    addView(subtitle_label_);
    auto* badge = ui::MakeLabel(display_.in_library ? "● " + Tr("In library") : display_.badge,
        12, display_.in_library ? ui::Green() : ui::Muted());
    badge->setId("catalog-card-badge");
    badge->setWidthPercentage(100);
    badge->setHeight(18);
    badge->setSingleLine(true);
    addView(badge);

    UpdateChrome(false);
    LoadImage();

    registerClickAction([this](brls::View* view) {
        (void)view;
        if (click_handler_)
            click_handler_();

        return true;
    });
    addGestureRecognizer(new brls::TapGestureRecognizer(this));
    updateActionHint(brls::BUTTON_A, Tr("View game"));
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
    setBackgroundColor(focused ? ui::Raised() : ui::Ground());
    setBorderColor(focused ? ui::Green() : nvgRGBA(42, 45, 51, 0));
}

void GameCardView::LoadImage()
{
    SetCachedCoverImage(image_, display_.image_url);
}

} // namespace opennow
