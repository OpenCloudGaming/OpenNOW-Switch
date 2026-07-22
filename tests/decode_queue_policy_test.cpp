#include "stream/DecodeQueuePolicy.hpp"

#include <cassert>

static_assert(opennow::video::MaximumQueuedAccessUnits(30) == 2);
static_assert(opennow::video::MaximumQueuedAccessUnits(60) == 4);
static_assert(opennow::video::MaximumQueuedAccessUnits(120) == 4);

int main()
{
    assert(opennow::video::MaximumQueuedAccessUnits(1) == 2);
    assert(opennow::video::MaximumQueuedAccessUnits(60) < 8);
    return 0;
}
