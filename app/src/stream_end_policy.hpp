#pragma once

#include <chrono>

namespace opennow
{

enum class PeerTerminalKind
{
    None,
    Failed,
    Disconnected,
    Closed,
};

enum class StreamEndReason
{
    None,
    FreeSessionEnded,
    NetworkLost,
    ServerDisconnected,
    ConnectionFailed,
    StreamEnded,
    VideoTimedOut,
};

struct StreamEndSignals
{
    bool free_tier = false;
    std::chrono::seconds session_elapsed {0};
    bool internet_connected = true;
    bool peer_completed = false;
    PeerTerminalKind peer_terminal = PeerTerminalKind::None;
    bool signaling_connected = true;
    bool video_started = false;
    std::chrono::milliseconds video_idle {0};
};

inline StreamEndReason DetectStreamEnd(const StreamEndSignals& signals)
{
    constexpr auto kFreeLimit = std::chrono::hours(1);
    constexpr auto kFreeTerminalGrace = std::chrono::minutes(59);
    if (signals.free_tier &&
        (signals.session_elapsed >= kFreeLimit ||
         (signals.session_elapsed >= kFreeTerminalGrace &&
          signals.peer_terminal != PeerTerminalKind::None)))
    {
        return StreamEndReason::FreeSessionEnded;
    }

    if (!signals.internet_connected &&
        (signals.peer_completed || signals.video_started ||
         signals.peer_terminal != PeerTerminalKind::None))
    {
        return StreamEndReason::NetworkLost;
    }

    switch (signals.peer_terminal)
    {
        case PeerTerminalKind::Failed:
            return signals.peer_completed
                ? StreamEndReason::ServerDisconnected
                : StreamEndReason::ConnectionFailed;
        case PeerTerminalKind::Disconnected:
            return StreamEndReason::ServerDisconnected;
        case PeerTerminalKind::Closed:
            return StreamEndReason::StreamEnded;
        case PeerTerminalKind::None:
            break;
    }

    if (signals.peer_completed && !signals.signaling_connected &&
        signals.video_started && signals.video_idle >= std::chrono::seconds(8))
    {
        return StreamEndReason::ServerDisconnected;
    }

    if (signals.video_started && signals.video_idle >= std::chrono::seconds(15))
        return StreamEndReason::VideoTimedOut;

    return StreamEndReason::None;
}

} // namespace opennow
