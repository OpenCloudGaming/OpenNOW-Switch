#pragma once

#include <algorithm>
#include <initializer_list>
#include <vector>

namespace opennow::stream {

inline std::vector<int> PrioritizeRemoteCandidatePorts(
    int session_media_port, std::initializer_list<int> fallback_ports) {
    std::vector<int> ports;
    auto add_unique = [&ports](int port) {
        if (port <= 0 || port > 65535)
            return;
        if (std::find(ports.begin(), ports.end(), port) == ports.end())
            ports.push_back(port);
    };

    // Port 443 belongs to the WSS control endpoint unless the media SDP
    // explicitly advertises it as a fallback.
    if (session_media_port != 443)
        add_unique(session_media_port);
    for (int port : fallback_ports)
        add_unique(port);
    return ports;
}

} // namespace opennow::stream
