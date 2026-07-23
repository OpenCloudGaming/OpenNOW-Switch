#ifndef ICE_CANDIDATE_PAIR_POLICY_H_
#define ICE_CANDIDATE_PAIR_POLICY_H_

#include <stdint.h>

static inline uint64_t ice_candidate_pair_priority(
    uint32_t local_priority,
    uint32_t remote_priority,
    int local_is_controlling) {
  const uint32_t controlling_priority =
      local_is_controlling ? local_priority : remote_priority;
  const uint32_t controlled_priority =
      local_is_controlling ? remote_priority : local_priority;
  const uint32_t minimum =
      controlling_priority < controlled_priority
          ? controlling_priority
          : controlled_priority;
  const uint32_t maximum =
      controlling_priority > controlled_priority
          ? controlling_priority
          : controlled_priority;

  return ((uint64_t)minimum << 32) +
      ((uint64_t)maximum << 1) +
      (controlling_priority > controlled_priority ? 1u : 0u);
}

#endif  // ICE_CANDIDATE_PAIR_POLICY_H_
