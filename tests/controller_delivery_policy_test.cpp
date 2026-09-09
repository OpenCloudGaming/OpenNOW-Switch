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

    using opennow::input::ControllerDeliveryState;
    using opennow::input::ControllerReportBitmap;
    std::array<ControllerDeliveryState, 4> delivery {};
    const std::array<bool, 4> connected = {true, true, false, false};
    auto& first = delivery[0];
    first.initialized = true;
    first.plus_was_down = true;
    first.plus_long_press = true;
    first.last_buttons = 0xffff;
    first.last_left_trigger = 0xff;
    first.last_right_trigger = 0xff;
    first.last_lx = 123;
    first.last_ly = -123;
    first.last_rx = 456;
    first.last_ry = -456;
    first.plus_pressed_at = start;
    first.last_report = start;
    first.start_pulse.Queue(start);
    first.Reset(true);
    assert(first.pending_disconnect);
    assert(!first.initialized);
    assert(!first.plus_was_down && !first.plus_long_press);
    assert(first.last_buttons == 0);
    assert(first.last_left_trigger == 0 && first.last_right_trigger == 0);
    assert(first.last_lx == 0 && first.last_ly == 0 && first.last_rx == 0 && first.last_ry == 0);
    assert(first.plus_pressed_at.time_since_epoch().count() == 0);
    assert(first.last_report.time_since_epoch().count() == 0);
    assert(!first.start_pulse.IsActive(start));
    assert(ControllerReportBitmap(connected, delivery) == 0x0202);
    for (int retry = 0; retry < 100; ++retry)
    {
        first.Reset();
        assert(first.pending_disconnect);
        assert(ControllerReportBitmap(connected, delivery) == 0x0202);
    }
    first.pending_disconnect = false;
    assert(ControllerReportBitmap(connected, delivery) == 0x0303);
    first.Reset();
    assert(!first.pending_disconnect);
    delivery[0].Reset(true);
    delivery[1].Reset(true);
    assert(ControllerReportBitmap(connected, delivery) == 0);
    delivery[1].pending_disconnect = false;
    assert(ControllerReportBitmap(connected, delivery) == 0x0202);
    return 0;
}
