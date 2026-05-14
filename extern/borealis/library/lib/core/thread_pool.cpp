/*
    Copyright 2025 XITRIX

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <borealis/core/thread_pool.hpp>
#include <borealis/core/logger.hpp>
#include <algorithm>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace brls {

#if defined(__SWITCH__)
namespace {

unsigned defaultThreadPoolSize() {
    unsigned concurrency = std::thread::hardware_concurrency();
    if (concurrency == 0) {
        concurrency = 4;
    }

    // Keep a few workers available, but avoid oversubscribing a 4-core system.
    return std::min<unsigned>(3, std::max<unsigned>(2, concurrency - 1));
}

int countAllowedCores(u64 affinityMask) {
    int count = 0;
    for (s32 core = 0; core < 4; core++) {
        if (affinityMask & (1ULL << core)) {
            count++;
        }
    }
    return count;
}

s32 selectAllowedCore(u64 affinityMask, int ordinal) {
    s32 lastAllowedCore = -1;
    for (s32 core = 0; core < 4; core++) {
        if ((affinityMask & (1ULL << core)) == 0) {
            continue;
        }

        lastAllowedCore = core;
        if (ordinal == 0) {
            return core;
        }

        ordinal--;
    }

    return lastAllowedCore;
}

void applySwitchThreadPoolHints(int workerIndex) {
    s32 preferredCore = -1;
    u64 affinityMask = 0;
    if (R_FAILED(svcGetThreadCoreMask(&preferredCore, &affinityMask, CUR_THREAD_HANDLE))) {
        return;
    }

    const int allowedCoreCount = countAllowedCores(affinityMask);
    if (allowedCoreCount <= 0) {
        return;
    }

    int targetOrdinal = workerIndex % allowedCoreCount;
    if (allowedCoreCount > 2 && targetOrdinal == allowedCoreCount - 1) {
        targetOrdinal = allowedCoreCount - 2;
    }

    s32 targetCore = selectAllowedCore(affinityMask, targetOrdinal);
    if (targetCore >= 0 && targetCore != preferredCore) {
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, targetCore, static_cast<u32>(affinityMask));
    }

    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3A);
}

} // namespace
#else
namespace {

unsigned defaultThreadPoolSize() {
    return 8;
}

} // namespace
#endif

// Create global ThreadPool with default amount of threads (probably should be configurable)
ThreadPool* ThreadPool::_global = new ThreadPool(defaultThreadPoolSize());

ThreadPool::ThreadPool(int threads) : shutdown_(false) {
    // Create the specified number of threads
    threads_.reserve(threads);
    for (int i = 0; i < threads; ++i)
        threads_.emplace_back([this, i] { threadEntry(i); });
}

ThreadPool::~ThreadPool() {
    {
        // Unblock any threads and tell them to stop
        std::unique_lock<std::mutex> l(lock_);

        shutdown_ = true;
        condVar_.notify_all();
    }

    // Wait for all threads to stop
    brls::Logger::info("Joining threads");
    for (auto &thread: threads_)
        thread.join();
}

void ThreadPool::async(std::function<void(void)> func) {
    // Place a job on the queue and unblock a thread
    std::unique_lock<std::mutex> l(lock_);

    if (shutdown_)
        return;

    jobs_.emplace(std::move(func));
    condVar_.notify_one();
}

void ThreadPool::shutdownGlobal() {
    ThreadPool* global = _global;
    if (!global)
        return;

    _global = nullptr;
    delete global;
}

void ThreadPool::threadEntry(int i) {
    std::function<void(void)> job;

#if defined(__SWITCH__)
    applySwitchThreadPoolHints(i);
#endif

    while (true) {
        {
            std::unique_lock<std::mutex> l(lock_);

            while (!shutdown_ && jobs_.empty())
                condVar_.wait(l);

            if (jobs_.empty()) {
                // No jobs to do and we are shutting down
                brls::Logger::info("Thread {} terminates", i);
                return;
            }

//            brls::Logger::info("Thread {} does a job", i);
            job = std::move(jobs_.front());
            jobs_.pop();
        }

        // Do the job without holding any locks
        job();
    }

}

} // namespace brls
