#pragma once

#include "controller_assignment_policy.hpp"

#include <chrono>

namespace opennow::input
{

class StartDeliveryPulse
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
    uint16_t last_buttons = 0;
    uint8_t last_left_trigger = 0;
    uint8_t last_right_trigger = 0;
    int16_t last_lx = 0;
    int16_t last_ly = 0;
    int16_t last_rx = 0;
    int16_t last_ry = 0;
    std::chrono::steady_clock::time_point plus_pressed_at {};
    std::chrono::steady_clock::time_point last_report {};
    StartDeliveryPulse start_pulse;

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

} // namespace opennow::input
