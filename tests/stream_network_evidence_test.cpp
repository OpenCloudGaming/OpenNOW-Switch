#include "stream_end_policy.hpp"

#include <cassert>
#include <chrono>

using namespace std::chrono_literals;

int main()
{
    using namespace opennow;
    StreamEndSignals signals;
    signals.peer_completed = true;
    signals.video_started = true;

    const auto detect = [&](bool internet_connected) {
        signals.internet_connected = HasStreamNetworkConnection(
            internet_connected, signals.video_started, signals.video_idle);
        return DetectStreamEnd(signals);
    };

    for (const auto idle : {0ms, 16ms, 1000ms, 2000ms, 2999ms})
    {
        signals.video_idle = idle;
        assert(detect(false) == StreamEndReason::None);
    }

    signals.video_idle = 3000ms;
    assert(detect(false) == StreamEndReason::NetworkLost);
    assert(detect(true) == StreamEndReason::None);

    signals.video_idle = 0ms;
    for (int sample = 0; sample < 30; ++sample)
        assert(detect(false) == StreamEndReason::None);
    assert(detect(true) == StreamEndReason::None);

    signals.peer_terminal = PeerTerminalKind::Failed;
    assert(detect(false) == StreamEndReason::ServerDisconnected);
    signals.peer_terminal = PeerTerminalKind::Disconnected;
    assert(detect(false) == StreamEndReason::ServerDisconnected);
    signals.peer_terminal = PeerTerminalKind::Closed;
    assert(detect(false) == StreamEndReason::StreamEnded);
    signals.peer_terminal = PeerTerminalKind::None;

    signals.signaling_connected = false;
    signals.video_idle = 7999ms;
    assert(detect(true) == StreamEndReason::None);
    signals.video_idle = 8000ms;
    assert(detect(true) == StreamEndReason::ServerDisconnected);
    signals.signaling_connected = true;
    signals.video_idle = 14999ms;
    assert(detect(true) == StreamEndReason::None);
    signals.video_idle = 15000ms;
    assert(detect(true) == StreamEndReason::VideoTimedOut);

    signals.video_idle = 0ms;
    signals.free_tier = true;
    signals.session_elapsed = 1h;
    assert(detect(false) == StreamEndReason::FreeSessionEnded);

    signals = {};
    assert(detect(false) == StreamEndReason::None);
    signals.peer_completed = true;
    assert(detect(false) == StreamEndReason::NetworkLost);
    assert(detect(true) == StreamEndReason::None);
    signals.peer_completed = false;
    signals.peer_terminal = PeerTerminalKind::Failed;
    assert(detect(true) == StreamEndReason::ConnectionFailed);
}
