#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace opennow::input
{

constexpr std::size_t kRemoteControllerCount = 4;
constexpr std::size_t kSwitchControllerSourceCount = 5;
constexpr std::int8_t kUnassignedController = -1;

class ControllerAssignments
{
  public:
    ControllerAssignments()
    {
        source_to_controller_.fill(kUnassignedController);
        controller_to_source_.fill(kUnassignedController);
    }

    std::int8_t Assign(std::size_t source)
    {
        if (source >= source_to_controller_.size())
            return kUnassignedController;
        if (source_to_controller_[source] != kUnassignedController)
            return source_to_controller_[source];

        for (std::size_t controller = 0;
             controller < controller_to_source_.size(); ++controller)
        {
            if (controller_to_source_[controller] == kUnassignedController)
                return Bind(source, controller);
        }
        return kUnassignedController;
    }

    void Release(std::size_t source)
    {
        const auto controller = ControllerForSource(source);
        if (controller == kUnassignedController)
            return;
        controller_to_source_[static_cast<std::size_t>(controller)] = kUnassignedController;
        source_to_controller_[source] = kUnassignedController;
    }

    std::array<bool, kRemoteControllerCount> Update(
        const std::array<bool, kSwitchControllerSourceCount>& connected)
    {
        std::array<bool, kRemoteControllerCount> released {};
        for (std::size_t source = 0; source < connected.size(); ++source)
        {
            const auto controller = ControllerForSource(source);
            if (!connected[source] && controller != kUnassignedController)
            {
                released[static_cast<std::size_t>(controller)] = true;
                Release(source);
            }
        }
        for (std::size_t source = 0; source < connected.size(); ++source)
        {
            if (connected[source])
                Assign(source);
        }
        return released;
    }

    std::int8_t ControllerForSource(std::size_t source) const
    {
        return source < source_to_controller_.size()
            ? source_to_controller_[source]
            : kUnassignedController;
    }

  private:
    std::int8_t Bind(std::size_t source, std::size_t controller)
    {
        source_to_controller_[source] = static_cast<std::int8_t>(controller);
        controller_to_source_[controller] = static_cast<std::int8_t>(source);
        return static_cast<std::int8_t>(controller);
    }

    std::array<std::int8_t, kSwitchControllerSourceCount> source_to_controller_;
    std::array<std::int8_t, kRemoteControllerCount> controller_to_source_;
};

inline std::uint16_t ControllerBitmap(
    const std::array<bool, kRemoteControllerCount>& connected)
{
    std::uint16_t bitmap = 0;
    for (std::size_t controller = 0; controller < connected.size(); ++controller)
    {
        if (!connected[controller])
            continue;
        bitmap |= static_cast<std::uint16_t>(1u << controller);
        bitmap |= static_cast<std::uint16_t>(1u << (controller + 8));
    }
    return bitmap;
}

} // namespace opennow::input
