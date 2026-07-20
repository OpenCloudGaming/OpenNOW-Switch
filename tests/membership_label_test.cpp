#include "membership_label.hpp"

#include <cassert>

int main()
{
    using opennow::BuildMembershipBadge;

    assert(BuildMembershipBadge("In library", "For Premium Members") ==
           "In library / For Premium Members");
    assert(BuildMembershipBadge("In library", "") == "In library");
    assert(BuildMembershipBadge("", "For Premium Members") == "For Premium Members");
    return 0;
}
