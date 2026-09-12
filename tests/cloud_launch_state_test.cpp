#include "cloud_launch_state.hpp"

#include <cassert>
#include <latch>
#include <thread>

int main()
{
    {
        opennow::CloudLaunchState state;
        assert(state.Cancel().empty());
        assert(!state.running());
        assert(!state.Adopt("late-session"));
        assert(state.TakeForCleanup().empty());
    }
    {
        opennow::CloudLaunchState state;
        assert(state.Adopt("queued-session"));
        assert(state.Cancel() == "queued-session");
        assert(state.Cancel().empty());
        assert(state.TakeForCleanup().empty());
    }
    {
        opennow::CloudLaunchState state;
        assert(state.Adopt("failed-session"));
        assert(state.TakeForCleanup() == "failed-session");
        assert(state.running());
        assert(state.Cancel().empty());
    }
    {
        opennow::CloudLaunchState state;
        assert(state.Adopt("playing-session"));
        state.TransferToStream();
        assert(!state.running());
        assert(state.Cancel().empty());
        assert(state.TakeForCleanup().empty());
    }
    {
        opennow::CloudLaunchState state;
        std::latch allocating(1);
        std::latch complete(1);
        int stopped = 0;
        std::thread worker([&] {
            allocating.count_down();
            complete.wait();
            if (!state.Adopt("allocated-after-cancel"))
                ++stopped;
        });
        allocating.wait();
        assert(state.Cancel().empty());
        complete.count_down();
        worker.join();
        assert(stopped == 1);
    }
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        opennow::CloudLaunchState state;
        std::atomic<int> stopped {0};
        std::latch start(1);
        std::thread worker([&] {
            start.wait();
            if (!state.Adopt("racing-session"))
                ++stopped;
        });
        start.count_down();
        if (!state.Cancel().empty())
            ++stopped;
        worker.join();
        assert(stopped == 1);
    }
}
