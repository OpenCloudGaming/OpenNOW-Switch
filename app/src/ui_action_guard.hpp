#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>

namespace opennow
{

void LogUiAction(const std::string& action, const std::string& phase,
                 const std::string& detail = "");
bool RunUiAction(const std::string& action, const std::function<void()>& callback);
bool IsViewInside(brls::View* root, brls::View* candidate);
void MoveFocusBeforeDestroy(brls::View* root, brls::View* stable_target);

} // namespace opennow
