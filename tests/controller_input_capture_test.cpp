#include "controller_delivery_policy.hpp"

#include <cassert>
#include <chrono>
#include <vector>

int main()
{
    using namespace std::chrono_literals;
    using namespace opennow::input;
    const auto start = std::chrono::steady_clock::time_point {} + 1s;
    std::array<bool, kRemoteControllerCount> connected {true, true, false, false};
    std::array<ControllerDeliveryState, kRemoteControllerCount> delivery {};
    ControllerInputCapture capture;
    delivery[0].initialized = true;
    delivery[0].last_buttons = 0x0100;
    delivery[0].last_lx = 20000;
    delivery[0].ObserveSystemButtons(true, false, start);
    delivery[1].back_pulse.Queue(start);
    delivery[2].Reset(true);

    assert(capture.Update(true, delivery));
    assert(!delivery[0].initialized);
    assert(delivery[0].last_buttons == 0 && delivery[0].last_lx == 0);
    assert(delivery[0].SystemButtons(start) == 0);
    assert(delivery[1].SystemButtons(start) == 0);
    assert(delivery[2].pending_disconnect);

    struct Report
    {
        uint8_t controller;
        uint16_t bitmap;
    };
    std::vector<Report> attempts;
    std::array<bool, kRemoteControllerCount> succeeds {false, true, false, true};
    const auto send = [&](uint8_t controller, uint16_t bitmap) {
        attempts.push_back({controller, bitmap});
        return succeeds[controller];
    };
    DeliverNeutralControllerReports(connected, delivery, start, send);
    assert(attempts.size() == 3);
    assert(attempts[0].controller == 0 && attempts[0].bitmap == 0x0303);
    assert(attempts[1].controller == 1 && attempts[1].bitmap == 0x0303);
    assert(attempts[2].controller == 2 && attempts[2].bitmap == 0x0303);
    assert(delivery[0].last_report.time_since_epoch().count() == 0);
    assert(delivery[1].last_report == start);
    assert(delivery[2].pending_disconnect);

    assert(!capture.Update(true, delivery));
    DeliverNeutralControllerReports(connected, delivery, start + 99ms, send);
    assert(attempts.size() == 3);

    succeeds.fill(true);
    DeliverNeutralControllerReports(connected, delivery, start + 100ms, send);
    assert(attempts.size() == 6);
    assert(delivery[0].last_report == start + 100ms);
    assert(!delivery[2].pending_disconnect);
    DeliverNeutralControllerReports(connected, delivery, start + 200ms, send);
    assert(attempts.size() == 8);

    connected[3] = true;
    delivery[3].Reset();
    DeliverNeutralControllerReports(connected, delivery, start + 201ms, send);
    assert(attempts.size() == 9);
    assert(attempts.back().controller == 3 && attempts.back().bitmap == 0x0b0b);
    assert(!capture.Update(true, delivery));
    DeliverNeutralControllerReports(connected, delivery, start + 250ms, send);
    assert(attempts.size() == 9);

    assert(capture.Update(false, delivery));
    assert(!delivery[0].initialized);
    delivery[0].ObserveSystemButtons(true, false, start + 250ms);
    delivery[0].ObserveSystemButtons(false, false, start + 330ms);
    assert(delivery[0].SystemButtons(start + 330ms) == 0x0010);
    assert(capture.Update(true, delivery));
    assert(delivery[0].SystemButtons(start + 330ms) == 0);
    DeliverNeutralControllerReports(connected, delivery, start + 330ms, send);
    assert(attempts.size() == 12);

    connected.fill(false);
    delivery[1].Reset(true);
    succeeds[1] = false;
    DeliverNeutralControllerReports(connected, delivery, start + 331ms, send);
    assert(attempts.size() == 13 && attempts.back().controller == 1);
    assert(delivery[1].pending_disconnect);
    assert(capture.Update(false, delivery));
    assert(delivery[1].pending_disconnect);

    connected = {true, false, false, false};
    delivery = {};
    attempts.clear();
    succeeds.fill(false);
    assert(capture.Update(true, delivery));
    for (int milliseconds = 0; milliseconds < 1000; ++milliseconds)
    {
        assert(!capture.Update(true, delivery));
        DeliverNeutralControllerReports(
            connected, delivery, start + std::chrono::milliseconds(milliseconds), send);
    }
    assert(attempts.size() == 10);
    succeeds.fill(true);
    for (int milliseconds = 1000; milliseconds < 2000; ++milliseconds)
    {
        DeliverNeutralControllerReports(
            connected, delivery, start + std::chrono::milliseconds(milliseconds), send);
    }
    assert(attempts.size() == 20);
}
