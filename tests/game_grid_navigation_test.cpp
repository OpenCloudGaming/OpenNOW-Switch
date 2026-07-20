#include "grid_navigation_policy.hpp"

#include <cassert>

static_assert(opennow::GridTargetColumn(0, 5) == 0);
static_assert(opennow::GridTargetColumn(3, 5) == 3);
static_assert(opennow::GridTargetColumn(4, 3) == 2);
static_assert(opennow::GridTargetColumn(8, 1) == 0);
static_assert(opennow::GridTargetColumn(2, 4) == 2);

int main()
{
    assert(opennow::GridTargetColumn(2, 5) == 2);
    assert(opennow::GridTargetColumn(4, 2) == 1);
    // Account row (3), toolbar row (4), and game row (5) preserve the
    // horizontal intent while clamping only at a shorter row boundary.
    assert(opennow::GridTargetColumn(3, 3) == 2);
    assert(opennow::GridTargetColumn(2, 4) == 2);
    assert(opennow::GridTargetColumn(2, 5) == 2);
    return 0;
}
