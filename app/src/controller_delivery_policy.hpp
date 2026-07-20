#pragma once

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

} // namespace opennow::input
