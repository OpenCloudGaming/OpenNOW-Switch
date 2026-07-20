#include "stream/RemoteCandidatePolicy.hpp"

#include <cassert>

int main() {
    using opennow::stream::PrioritizeRemoteCandidatePorts;

    const auto dynamic = PrioritizeRemoteCandidatePorts(48322, {47998, 47998, 47999});
    assert((dynamic == std::vector<int> {48322, 47998, 47999}));

    const auto control_port = PrioritizeRemoteCandidatePorts(443, {47998, 47999});
    assert((control_port == std::vector<int> {47998, 47999}));

    const auto validated = PrioritizeRemoteCandidatePorts(0, {-1, 47998, 70000, 47999});
    assert((validated == std::vector<int> {47998, 47999}));
    return 0;
}
