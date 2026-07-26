#include "gfn/app_launch_mode_policy.hpp"

#include <cassert>

int main()
{
    assert(opennow::gfn::AppLaunchModeWireValue(false) == 1);
    assert(opennow::gfn::AppLaunchModeWireValue(true) == 2);
    return 0;
}
