#include "ui_refresh_policy.hpp"

#include <cassert>
#include <chrono>

static_assert(opennow::ui::ShouldRefreshStatus(std::chrono::milliseconds(0), false));
static_assert(!opennow::ui::ShouldRefreshStatus(std::chrono::milliseconds(999), true));
static_assert(opennow::ui::ShouldRefreshStatus(std::chrono::milliseconds(1000), true));

int main()
{
    using namespace std::chrono_literals;
    assert(!opennow::ui::ShouldRefreshStatus(100ms, true));
    assert(opennow::ui::ShouldRefreshStatus(1s, true));
    return 0;
}
