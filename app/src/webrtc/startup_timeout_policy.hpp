#pragma once

#include <chrono>

namespace opennow::webrtc
{

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
        return session_elapsed >= std::chrono::seconds(30)
            ? StartupTimeout::Transport : StartupTimeout::None;
    if (!frame_decoded && peer_completed_elapsed >= std::chrono::seconds(15))
        return StartupTimeout::Video;
    return StartupTimeout::None;
}

}
