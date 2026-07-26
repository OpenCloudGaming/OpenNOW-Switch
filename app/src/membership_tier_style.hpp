#pragma once

#include "membership_tier_policy.hpp"

#include <borealis.hpp>

#include <string>

namespace opennow::membership
{

inline NVGcolor TextColor(const std::string& tier)
{
    switch (AccentForTier(tier))
    {
        case TierAccent::Ultimate:
            return nvgRGB(255, 215, 0);
        case TierAccent::Performance:
            return nvgRGB(205, 175, 149);
        case TierAccent::Neutral:
            return nvgRGB(112, 119, 130);
    }

    return nvgRGB(112, 119, 130);
}

} // namespace opennow::membership
