#pragma once

#include <chrono>

namespace opennow::webrtc
{

inline constexpr auto TransportStartupTimeout = std::chrono::seconds(30);

enum class StartupTimeout
{
    None,
    Transport,
    Video,
};

constexpr StartupTimeout DetectStartupTimeout(
    bool peer_completed,
    bool frame_decoded,
    std::chrono::milliseconds session_elapsed,
    std::chrono::milliseconds peer_completed_elapsed)
{
    if (!peer_completed)
        return session_elapsed >= TransportStartupTimeout
            ? StartupTimeout::Transport : StartupTimeout::None;
    if (!frame_decoded && peer_completed_elapsed >= std::chrono::seconds(15))
        return StartupTimeout::Video;
    return StartupTimeout::None;
}

}
