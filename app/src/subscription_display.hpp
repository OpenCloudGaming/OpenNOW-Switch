#pragma once

#include "models.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace opennow::subscription
{

inline std::string FormatDecimal(double value)
{
    std::ostringstream output;
    output << std::fixed << std::setprecision(
        std::abs(value - std::round(value)) < 0.05 ? 0 : 1) << value;
    return output.str();
}

inline std::string FormatTimeRemaining(const SubscriptionInfo& info)
{
    if (info.is_unlimited)
        return "Unlimited";

    const int minutes = std::max(
        0, static_cast<int>(std::round(info.remaining_hours * 60.0)));
    const int hours = minutes / 60;
    const int remainder = minutes % 60;
    if (hours == 0)
        return std::to_string(remainder) + "m left";
    if (remainder == 0)
        return std::to_string(hours) + "h left";
    return std::to_string(hours) + "h " + std::to_string(remainder) + "m left";
}

inline std::string FormatStorageRemaining(const SubscriptionInfo& info)
{
    if (!info.has_storage)
        return "No storage";
    if (!info.has_storage_usage)
        return FormatDecimal(info.storage_size_gb) + " GB total";
    const double remaining = std::max(info.storage_size_gb - info.storage_used_gb, 0.0);
    return FormatDecimal(remaining) + " GB left";
}

inline std::string FormatStorageUsage(const SubscriptionInfo& info)
{
    if (!info.has_storage)
        return "Not included";
    if (!info.has_storage_usage)
        return FormatDecimal(info.storage_size_gb) + " GB total";
    return FormatDecimal(info.storage_used_gb) + " GB used of " +
        FormatDecimal(info.storage_size_gb) + " GB";
}

} // namespace opennow::subscription
