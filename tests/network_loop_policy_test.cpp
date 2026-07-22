#include "network_loop_policy.hpp"

#include <cassert>

static_assert(opennow::network::LoopBackoffMilliseconds(false, 0) == 2);
static_assert(opennow::network::LoopBackoffMilliseconds(true, 0) == 1);
static_assert(opennow::network::LoopBackoffMilliseconds(true, 1) == 0);
static_assert(opennow::network::LoopBackoffMilliseconds(true, 24) == 0);

int main()
{
    using opennow::network::LoopBackoffMilliseconds;
    assert(LoopBackoffMilliseconds(false, 0) > LoopBackoffMilliseconds(true, 0));
    assert(LoopBackoffMilliseconds(true, 8) == 0);
    return 0;
}
