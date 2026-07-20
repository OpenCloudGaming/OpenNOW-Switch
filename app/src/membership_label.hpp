#pragma once

#include <string>

namespace opennow
{

inline std::string BuildMembershipBadge(const std::string& current_badge,
                                        const std::string& membership_label)
{
    if (membership_label.empty())
        return current_badge;
    if (current_badge.empty())
        return membership_label;
    return current_badge + " / " + membership_label;
}

} // namespace opennow
