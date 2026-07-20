#include "controller_delivery_policy.hpp"

#include <cassert>
#include <chrono>

int main()
{
    using namespace std::chrono_literals;
    using opennow::input::StartDeliveryPulse;

    const auto start = StartDeliveryPulse::Clock::now();
    StartDeliveryPulse pulse;
    pulse.Queue(start);
    assert(pulse.IsActive(start));
    assert(pulse.IsActive(start + 1500ms));
    assert(pulse.OnReportDelivered(start + 1500ms));
    assert(pulse.IsActive(start + 1679ms));
    assert(!pulse.IsActive(start + 1680ms));

    StartDeliveryPulse expired;
    expired.Queue(start);
    assert(!expired.IsActive(start + 2s));
    assert(!expired.OnReportDelivered(start + 2s));
    return 0;
}
