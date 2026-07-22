#include "membership_tier_policy.hpp"

#include <cassert>

int main()
{
    using opennow::membership::DisplayLabel;
    assert(DisplayLabel("ULTIMATE", true) == "ULTIMATE");
    assert(DisplayLabel("FREE", true) == "FREE");
    assert(DisplayLabel("FREE", false) == "Membership unavailable");
    assert(DisplayLabel("", false) == "Membership unavailable");
    return 0;
}
