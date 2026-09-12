#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace opennow
{

template <typename Callback>
auto GuardStartupCallback(std::shared_ptr<std::atomic_bool> alive, Callback callback)
{
    return [alive = std::move(alive), callback = std::move(callback)]() mutable {
        if (alive->load())
            callback();
    };
}

} // namespace opennow
