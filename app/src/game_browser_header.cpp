#include "game_browser_header.hpp"

#include "localization.hpp"
#include "ui_text_policy.hpp"
#include "ui_theme.hpp"

namespace opennow::ui
{

brls::Button* MakeGameBrowserActionButton(const std::string& text)
{
    auto* button = new brls::Button();
    const std::string localized = Tr(text);
    button->setText(localized);
    button->setFontSize(15);
    button->setStyle(&brls::BUTTONSTYLE_BORDERED);
    button->setHeight(42);
    button->setWidth(ToolbarButtonWidth(localized));
    return button;
}

brls::Box* MakeGameBrowserHeader(
    const std::string& title, const std::vector<brls::View*>& actions)
{
    auto* header = new brls::Box(brls::Axis::ROW);
    header->setHeight(48);
    header->setAlignItems(brls::AlignItems::CENTER);
    header->setMarginBottom(8);

    auto* heading = new brls::Box(brls::Axis::ROW);
    heading->setGrow(1.0f);
    heading->setAlignItems(brls::AlignItems::CENTER);

    auto* label = new StyledLabel(FontRole::Heading);
    label->setId("browser-heading");
    label->setText(Tr(title));
    label->setFontSize(28);
    label->setTextColor(Text());
    heading->addView(label);
    header->addView(heading);

    for (brls::View* action : actions)
    {
        action->setMarginLeft(10);
        header->addView(action);
    }

    return header;
}

} // namespace opennow::ui
