#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace opennow
{
class CoverImageWorker
{
  public:
    static constexpr std::size_t kPendingLimit = 30;

    CoverImageWorker() : thread_([this] { Run(); }) {}
    ~CoverImageWorker() { Stop(); }

    bool TrySubmit(std::shared_ptr<std::atomic_bool> cancelled, std::function<void()> work)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
            return false;
        std::erase_if(pending_, [](const Job& job) { return job.cancelled->load(); });
        if (pending_.size() == kPendingLimit)
            return false;
        pending_.push_back({std::move(cancelled), std::move(work)});
        wake_.notify_one();
        return true;
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
            if (active_)
                active_->store(true);
            for (auto& job : pending_)
                job.cancelled->store(true);
            pending_.clear();
            wake_.notify_one();
        }
        if (thread_.joinable())
            thread_.join();
    }

  private:
    struct Job
    {
        std::shared_ptr<std::atomic_bool> cancelled;
        std::function<void()> work;
    };

    void Run()
    {
#ifdef __SWITCH__
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
#endif
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;)
        {
            wake_.wait(lock, [this] { return stopped_ || !pending_.empty(); });
            if (stopped_)
                return;
            Job job = std::move(pending_.front());
            pending_.pop_front();
            active_ = job.cancelled;
            lock.unlock();
            if (!job.cancelled->load())
            {
                try
                {
                    job.work();
                }
                catch (...)
                {
                }
            }
            lock.lock();
            active_.reset();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> pending_;
    std::shared_ptr<std::atomic_bool> active_;
    bool stopped_ = false;
    std::thread thread_;
};
}
