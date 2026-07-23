#pragma once

#include "home_shortcut_policy.hpp"

#include <borealis.hpp>
#include <optional>

namespace opennow
{

class MainActivity : public brls::Activity
{
  public:
    explicit MainActivity(
        std::optional<shortcut::LaunchRequest> launch_request = std::nullopt);
};

} // namespace opennow
