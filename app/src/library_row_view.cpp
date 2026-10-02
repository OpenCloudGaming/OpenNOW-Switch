#include "library_row_view.hpp"

#include "cover_image_cache.hpp"
#include "localization.hpp"
#include "ui_theme.hpp"

#include <borealis/core/touch/tap_gesture.hpp>

#include <algorithm>
#include <utility>

namespace opennow
{

LibraryRowView::LibraryRowView(LibraryRowDisplay display, std::function<void()> selected,
                             std::function<void()> opened)
    : brls::Box(brls::Axis::ROW), selected_(std::move(selected))
{
    setId("library-row-" + display.identity);
    setWidthPercentage(100);
    setHeight(72);
    setShrink(0);
    setPadding(8, 12, 8, 12);
    setAlignItems(brls::AlignItems::CENTER);
    setMarginBottom(2);
    setCornerRadius(12);
    setBorderThickness(1);
    setFocusable(true);
    setHideHighlightBackground(true);
    setHighlightPadding(3);
    setHighlightCornerRadius(14);

    time_ = ui::MakeLabel(display.time, 12, ui::Muted(), ui::FontRole::Medium);
    time_->setWidth(80);
    time_->setShrink(0);
    time_->setSingleLine(true);
    time_->setMarginRight(10);
    addView(time_);

    auto* image = new CachedImage();
    image->setWidth(112);
    image->setHeight(56);
    image->setShrink(0);
    image->setMarginRight(18);
    image->setCornerRadius(6);
    image->setScalingType(brls::ImageScalingType::FILL);
    SetCachedCoverImage(image, display.image_url);
    addView(image);

    auto* text = new brls::Box(brls::Axis::COLUMN);
    text->setGrow(1);
    text->setShrink(1);
    auto* title = ui::MakeLabel(display.title, 20, ui::Text(), ui::FontRole::Heading);
    title->setSingleLine(true);
    title->setWidthPercentage(100);
    text->addView(title);
    std::string detail = display.store;
    if (!display.membership.empty())
        detail += (detail.empty() ? "" : " · ") + display.membership;
    auto* subtitle = ui::MakeLabel(detail, 14, ui::Muted());
    subtitle->setWidthPercentage(100);
    subtitle->setSingleLine(true);
    text->addView(subtitle);
    addView(text);

    prompt_ = ui::MakeLabel("A  " + Tr("Open"), 14, ui::Green(), ui::FontRole::Medium);
    prompt_->setWidth(std::max(72.0f, ui::TextWidth("A  " + Tr("Open"), 14, ui::FontRole::Medium) + 4));
    prompt_->setMarginLeft(12);
    prompt_->setSingleLine(true);
    prompt_->setShrink(0);
    addView(prompt_);
    registerClickAction([selected = selected_, opened = std::move(opened)](brls::View*) {
        if (selected)
            selected();
        if (opened)
            opened();
        return true;
    });
    addGestureRecognizer(new brls::TapGestureRecognizer(this));
    updateActionHint(brls::BUTTON_A, Tr("Open"));
    UpdateChrome(false);
}

void LibraryRowView::onFocusGained()
{
    brls::Box::onFocusGained();
    UpdateChrome(true);
    if (selected_)
        selected_();
}

void LibraryRowView::onFocusLost()
{
    brls::Box::onFocusLost();
    UpdateChrome(false);
}

void LibraryRowView::UpdateChrome(bool focused)
{
    setBackgroundColor(focused ? ui::Raised() : ui::Ground());
    setBorderColor(focused ? ui::Green() : nvgRGBA(42, 45, 51, 0));
    time_->setTextColor(focused ? ui::Green() : ui::Muted());
    prompt_->setVisibility(focused ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
}

}
