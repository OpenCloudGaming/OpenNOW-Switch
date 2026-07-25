#include "stream_end_policy.hpp"

#include <cassert>

int main()
{
    using namespace opennow;
    StreamEndSignals signals;

    signals.free_tier = true;
    signals.session_elapsed = std::chrono::hours(1);
    assert(DetectStreamEnd(signals) == StreamEndReason::FreeSessionEnded);

    signals = {};
    signals.peer_completed = true;
    signals.video_started = true;
    signals.internet_connected = false;
    assert(DetectStreamEnd(signals) == StreamEndReason::NetworkLost);

    signals = {};
    signals.peer_completed = true;
    signals.peer_terminal = PeerTerminalKind::Disconnected;
    assert(DetectStreamEnd(signals) == StreamEndReason::ServerDisconnected);

    signals = {};
    signals.peer_terminal = PeerTerminalKind::Failed;
    assert(DetectStreamEnd(signals) == StreamEndReason::ConnectionFailed);

    signals = {};
    signals.peer_completed = true;
    signals.video_started = true;
    signals.signaling_connected = false;
    signals.video_idle = std::chrono::seconds(8);
    assert(DetectStreamEnd(signals) == StreamEndReason::ServerDisconnected);

    signals.signaling_connected = true;
    signals.video_idle = std::chrono::seconds(15);
    assert(DetectStreamEnd(signals) == StreamEndReason::VideoTimedOut);

    signals.video_idle = std::chrono::seconds(2);
    assert(DetectStreamEnd(signals) == StreamEndReason::None);

    assert(!ShouldProbeInternetConnection(
        PeerTerminalKind::None, true, std::chrono::seconds(7)));
    assert(ShouldProbeInternetConnection(
        PeerTerminalKind::None, true, std::chrono::seconds(8)));
    assert(ShouldProbeInternetConnection(
        PeerTerminalKind::Closed, false, std::chrono::milliseconds(0)));
    return 0;
}
