#pragma once

#include "controller_assignment_policy.hpp"

#include <chrono>

namespace opennow::input
{

class ControllerButtonPulse
{
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void Queue(TimePoint now)
    {
        pending_ = true;
        delivery_deadline_ = now + std::chrono::seconds(2);
        hold_until_ = {};
    }

    bool IsActive(TimePoint now)
    {
        if (pending_ && now >= delivery_deadline_)
            pending_ = false;
        return pending_ ||
               (hold_until_.time_since_epoch().count() != 0 && now < hold_until_);
    }

    bool OnReportDelivered(TimePoint now)
    {
        if (!pending_ || now >= delivery_deadline_)
            return false;
        pending_ = false;
        hold_until_ = now + std::chrono::milliseconds(180);
        return true;
    }

  private:
    bool pending_ = false;
    TimePoint delivery_deadline_ {};
    TimePoint hold_until_ {};
};

struct ControllerDeliveryState
{
    bool initialized = false;
    bool pending_disconnect = false;
    bool plus_was_down = false;
    bool plus_long_press = false;
    bool minus_was_down = false;
    bool minus_press_delivered = false;
    uint16_t last_buttons = 0;
    uint8_t last_left_trigger = 0;
    uint8_t last_right_trigger = 0;
    int16_t last_lx = 0;
    int16_t last_ly = 0;
    int16_t last_rx = 0;
    int16_t last_ry = 0;
    std::chrono::steady_clock::time_point plus_pressed_at {};
    std::chrono::steady_clock::time_point last_report {};
    std::chrono::steady_clock::time_point last_neutral_attempt {};
    ControllerButtonPulse start_pulse;
    ControllerButtonPulse back_pulse;

    void ObserveSystemButtons(
        bool plus_down, bool minus_down, std::chrono::steady_clock::time_point now)
    {
        if (plus_down && !plus_was_down)
        {
            plus_pressed_at = now;
            plus_long_press = false;
        }
        if (plus_down && !plus_long_press &&
            now - plus_pressed_at >= std::chrono::milliseconds(500))
        {
            plus_long_press = true;
        }
        if (!plus_down && plus_was_down && !plus_long_press)
        {
            start_pulse.Queue(now);
            initialized = false;
            last_report = {};
        }
        plus_was_down = plus_down;

        if (minus_down && !minus_was_down)
            minus_press_delivered = false;
        if (!minus_down && minus_was_down && !minus_press_delivered)
        {
            back_pulse.Queue(now);
            initialized = false;
            last_report = {};
        }
        minus_was_down = minus_down;
    }

    uint16_t SystemButtons(std::chrono::steady_clock::time_point now)
    {
        uint16_t buttons = 0;
        if (start_pulse.IsActive(now)) buttons |= 0x0010;
        if (minus_was_down || back_pulse.IsActive(now)) buttons |= 0x0020;
        if (plus_long_press && plus_was_down) buttons |= 0x0400;
        return buttons;
    }

    void OnSystemButtonsDelivered(std::chrono::steady_clock::time_point now)
    {
        if (minus_was_down)
            minus_press_delivered = true;
        if (start_pulse.OnReportDelivered(now))
            initialized = false;
        if (back_pulse.OnReportDelivered(now))
            initialized = false;
    }

    void Reset(bool disconnected = false)
    {
        const bool pending = pending_disconnect || disconnected;
        *this = {};
        pending_disconnect = pending;
    }
};

inline std::uint16_t ControllerReportBitmap(
    std::array<bool, kRemoteControllerCount> connected,
    const std::array<ControllerDeliveryState, kRemoteControllerCount>& delivery)
{
    for (std::size_t controller = 0; controller < connected.size(); ++controller)
    {
        if (delivery[controller].pending_disconnect)
            connected[controller] = false;
    }
    return ControllerBitmap(connected);
}

class ControllerInputCapture
{
  public:
    bool Update(
        bool captured,
        std::array<ControllerDeliveryState, kRemoteControllerCount>& delivery)
    {
        if (captured == captured_)
            return false;
        captured_ = captured;
        for (auto& state : delivery)
            state.Reset();
        return true;
    }

  private:
    bool captured_ = false;
};

template <typename SendNeutralReport>
void DeliverNeutralControllerReports(
    const std::array<bool, kRemoteControllerCount>& connected,
    std::array<ControllerDeliveryState, kRemoteControllerCount>& delivery,
    std::chrono::steady_clock::time_point now,
    SendNeutralReport send)
{
    const uint16_t bitmap = ControllerReportBitmap(connected, delivery);
    for (std::size_t controller = 0; controller < delivery.size(); ++controller)
    {
        auto& state = delivery[controller];
        if (!connected[controller] && !state.pending_disconnect)
            continue;
        if (state.last_neutral_attempt.time_since_epoch().count() != 0 &&
            now - state.last_neutral_attempt < std::chrono::milliseconds(100))
        {
            continue;
        }
        state.last_neutral_attempt = now;
        state.initialized = false;
        if (send(static_cast<uint8_t>(controller), bitmap))
        {
            state.pending_disconnect = false;
            state.last_report = now;
        }
    }
}

} // namespace opennow::input
