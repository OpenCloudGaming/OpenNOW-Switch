#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <utility>

namespace opennow
{

class CloudLaunchState
{
  public:
    bool running() const
    {
        return running_.load(std::memory_order_acquire);
    }

    bool Adopt(const std::string& session_id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_)
            return false;
        session_id_ = session_id;
        return true;
    }

    std::string Cancel()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_.store(false, std::memory_order_release);
        return std::exchange(session_id_, {});
    }

    std::string TakeForCleanup()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(session_id_, {});
    }

    void TransferToStream()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_id_.clear();
        running_.store(false, std::memory_order_release);
    }

  private:
    std::atomic<bool> running_ {true};
    std::mutex mutex_;
    std::string session_id_;
};

}
