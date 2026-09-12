#include "webrtc/startup_timeout_policy.hpp"

#include <cassert>
#include <chrono>

int main()
{
    using namespace std::chrono_literals;
    using opennow::webrtc::DetectStartupTimeout;
    using Timeout = opennow::webrtc::StartupTimeout;

    assert(DetectStartupTimeout(false, false, 29999ms, 0ms) == Timeout::None);
    assert(DetectStartupTimeout(false, false, 30s, 0ms) == Timeout::Transport);
    assert(DetectStartupTimeout(true, false, 30s, 1s) == Timeout::None);
    assert(DetectStartupTimeout(true, false, 43999ms, 14999ms) == Timeout::None);
    assert(DetectStartupTimeout(true, false, 44s, 15s) == Timeout::Video);
    assert(DetectStartupTimeout(true, false, 15s, 15s) == Timeout::Video);
    assert(DetectStartupTimeout(true, false, 1h, 1h) == Timeout::Video);
    assert(DetectStartupTimeout(true, true, 1h, 1h) == Timeout::None);
}
