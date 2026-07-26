#pragma once

#include <borealis.hpp>

#include <string>
#include <vector>

namespace opennow::ui
{

brls::Button* MakeGameBrowserActionButton(const std::string& text);
brls::Box* MakeGameBrowserHeader(
    const std::string& title, const std::vector<brls::View*>& actions);

} // namespace opennow::ui
