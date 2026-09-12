#include "controller_delivery_policy.hpp"
#include "stream_overlay_policy.hpp"

#include <cassert>
#include <chrono>

int main()
{
    using namespace std::chrono_literals;
    using opennow::input::ControllerButtonPulse;

    const auto start = ControllerButtonPulse::Clock::now();
    ControllerButtonPulse pulse;
    pulse.Queue(start);
    assert(pulse.IsActive(start));
    assert(pulse.IsActive(start + 1500ms));
    assert(pulse.OnReportDelivered(start + 1500ms));
    assert(pulse.IsActive(start + 1679ms));
    assert(!pulse.IsActive(start + 1680ms));

    ControllerButtonPulse expired;
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
    first.minus_was_down = true;
    first.minus_press_delivered = true;
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
    first.back_pulse.Queue(start);
    first.Reset(true);
    assert(first.pending_disconnect);
    assert(!first.initialized);
    assert(!first.plus_was_down && !first.plus_long_press);
    assert(!first.minus_was_down && !first.minus_press_delivered);
    assert(first.last_buttons == 0);
    assert(first.last_left_trigger == 0 && first.last_right_trigger == 0);
    assert(first.last_lx == 0 && first.last_ly == 0 && first.last_rx == 0 && first.last_ry == 0);
    assert(first.plus_pressed_at.time_since_epoch().count() == 0);
    assert(first.last_report.time_since_epoch().count() == 0);
    assert(!first.start_pulse.IsActive(start));
    assert(!first.back_pulse.IsActive(start));
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

    for (bool plus : {false, true})
    {
        const uint16_t expected = plus ? 0x0010 : 0x0020;
        ControllerDeliveryState tap;
        const auto pressed = opennow::input::EvaluateOverlayChord(!plus, plus, {}, 0);
        assert(pressed.preempt_input && !pressed.toggle_overlay);
        tap.ObserveSystemButtons(plus, !plus, start);

        const auto released = opennow::input::EvaluateOverlayChord(
            false, false, pressed.next_state, 80);
        assert(!released.preempt_input && !released.toggle_overlay);
        tap.ObserveSystemButtons(false, false, start + 80ms);
        assert(tap.SystemButtons(start + 80ms) == expected);
        assert(tap.SystemButtons(start + 1500ms) == expected);
        tap.OnSystemButtonsDelivered(start + 1500ms);
        assert(tap.SystemButtons(start + 1679ms) == expected);
        assert(tap.SystemButtons(start + 1680ms) == 0);

        ControllerDeliveryState failed;
        failed.ObserveSystemButtons(plus, !plus, start);
        failed.ObserveSystemButtons(false, false, start + 80ms);
        assert(failed.SystemButtons(start + 2079ms) == expected);
        assert(failed.SystemButtons(start + 2080ms) == 0);

        ControllerDeliveryState chord;
        chord.ObserveSystemButtons(plus, !plus, start);
        const auto combined = opennow::input::EvaluateOverlayChord(
            true, true, pressed.next_state, 80);
        assert(combined.preempt_input && combined.toggle_overlay);
        chord.Reset();
        chord.ObserveSystemButtons(false, false, start + 100ms);
        assert(chord.SystemButtons(start + 100ms) == 0);

        ControllerDeliveryState interrupted;
        interrupted.ObserveSystemButtons(plus, !plus, start);
        interrupted.ObserveSystemButtons(false, false, start + 80ms);
        interrupted.Reset(true);
        assert(interrupted.pending_disconnect);
        assert(interrupted.SystemButtons(start + 100ms) == 0);
    }

    ControllerDeliveryState held_plus;
    held_plus.ObserveSystemButtons(true, false, start);
    const auto plus_pressed = opennow::input::EvaluateOverlayChord(false, true, {}, 0);
    const auto plus_held = opennow::input::EvaluateOverlayChord(
        false, true, plus_pressed.next_state, 120);
    assert(!plus_held.preempt_input && !plus_held.toggle_overlay);
    held_plus.ObserveSystemButtons(true, false, start + 499ms);
    assert(held_plus.SystemButtons(start + 499ms) == 0);
    held_plus.ObserveSystemButtons(true, false, start + 500ms);
    assert(held_plus.SystemButtons(start + 500ms) == 0x0400);
    held_plus.OnSystemButtonsDelivered(start + 500ms);
    held_plus.ObserveSystemButtons(false, false, start + 600ms);
    assert(held_plus.SystemButtons(start + 600ms) == 0);

    ControllerDeliveryState held_minus;
    held_minus.ObserveSystemButtons(false, true, start);
    assert(held_minus.SystemButtons(start + 120ms) == 0x0020);
    held_minus.OnSystemButtonsDelivered(start + 120ms);
    held_minus.ObserveSystemButtons(false, false, start + 160ms);
    assert(held_minus.SystemButtons(start + 160ms) == 0);

    for (std::size_t controller = 0; controller < delivery.size(); ++controller)
    {
        for (auto& state : delivery)
            state.Reset();
        delivery[controller].ObserveSystemButtons(true, false, start);
        delivery[controller].ObserveSystemButtons(false, false, start + 80ms);
        for (std::size_t other = 0; other < delivery.size(); ++other)
        {
            assert(delivery[other].SystemButtons(start + 80ms) ==
                   (other == controller ? 0x0010 : 0));
        }
    }
    return 0;
}
