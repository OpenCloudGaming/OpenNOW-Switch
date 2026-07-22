#include "subscription_display.hpp"

#include <cassert>

int main()
{
    opennow::SubscriptionInfo info;
    info.available = true;
    info.remaining_hours = 1.5;
    assert(opennow::subscription::FormatTimeRemaining(info) == "1h 30m left");

    info.remaining_hours = 0.25;
    assert(opennow::subscription::FormatTimeRemaining(info) == "15m left");

    info.is_unlimited = true;
    assert(opennow::subscription::FormatTimeRemaining(info) == "Unlimited");

    info.has_storage = true;
    info.has_storage_usage = true;
    info.storage_size_gb = 100.0;
    info.storage_used_gb = 24.5;
    assert(opennow::subscription::FormatStorageRemaining(info) == "75.5 GB left");
    assert(opennow::subscription::FormatStorageUsage(info) == "24.5 GB used of 100 GB");

    return 0;
}
