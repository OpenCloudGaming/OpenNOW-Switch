#include "ice_candidate_pair_policy.h"

#include <cassert>
#include <cstdint>

int main()
{
    constexpr std::uint32_t host_priority = 2'130'706'431;
    constexpr std::uint32_t srflx_priority = 1'690'523'135;
    constexpr std::uint32_t relay_priority = 16'777'215;

    const std::uint64_t direct = ice_candidate_pair_priority(
        host_priority, host_priority, 1);
    const std::uint64_t reflexive = ice_candidate_pair_priority(
        host_priority, srflx_priority, 1);
    const std::uint64_t relayed = ice_candidate_pair_priority(
        host_priority, relay_priority, 1);

    assert(direct > reflexive);
    assert(reflexive > relayed);
    assert(
        ice_candidate_pair_priority(host_priority, srflx_priority, 1) !=
        ice_candidate_pair_priority(host_priority, srflx_priority, 0));
    return 0;
}
