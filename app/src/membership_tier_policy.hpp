#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace opennow::membership
{

enum class TierAccent
{
    Neutral,
    Performance,
    Ultimate,
};

inline std::string DisplayLabel(const std::string& tier, bool verified)
{
    if (!verified || tier.empty())
        return "Membership unavailable";
    return tier;
}

inline TierAccent AccentForTier(std::string tier)
{
    std::transform(tier.begin(), tier.end(), tier.begin(), [](unsigned char value) {
        return static_cast<char>(std::toupper(value));
    });

    if (tier == "ULTIMATE")
        return TierAccent::Ultimate;
    if (tier == "PERFORMANCE" || tier == "PRIORITY")
        return TierAccent::Performance;
    return TierAccent::Neutral;
}

} // namespace opennow::membership
