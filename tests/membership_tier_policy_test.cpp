#include "membership_tier_policy.hpp"

#include <cassert>

int main()
{
    using opennow::membership::DisplayLabel;
    assert(DisplayLabel("ULTIMATE", true) == "ULTIMATE");
    assert(DisplayLabel("FREE", true) == "FREE");
    assert(DisplayLabel("FREE", false) == "Membership unavailable");
    assert(DisplayLabel("", false) == "Membership unavailable");
    assert(opennow::membership::AccentForTier("ULTIMATE") ==
           opennow::membership::TierAccent::Ultimate);
    assert(opennow::membership::AccentForTier("ultimate") ==
           opennow::membership::TierAccent::Ultimate);
    assert(opennow::membership::AccentForTier("PERFORMANCE") ==
           opennow::membership::TierAccent::Performance);
    assert(opennow::membership::AccentForTier("PRIORITY") ==
           opennow::membership::TierAccent::Performance);
    assert(opennow::membership::AccentForTier("FREE") ==
           opennow::membership::TierAccent::Neutral);
    return 0;
}
