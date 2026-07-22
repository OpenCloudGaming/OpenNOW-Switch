#pragma once

#include <string>

namespace opennow::membership
{

inline std::string DisplayLabel(const std::string& tier, bool verified)
{
    if (!verified || tier.empty())
        return "Membership unavailable";
    return tier;
}

} // namespace opennow::membership
