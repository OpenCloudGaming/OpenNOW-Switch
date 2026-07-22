#include "stream/audio/AudioLatencyPolicy.hpp"

#include <cassert>

static_assert(opennow::audio::ClampTargetBufferMs(5) == 30);
static_assert(opennow::audio::ClampTargetBufferMs(140) == 100);
static_assert(opennow::audio::PrimePacketCount(40) == 4);
static_assert(opennow::audio::InitialJitterHoldUs(40) == 40000);
static_assert(opennow::audio::ResyncJitterHoldUs(40) == 30000);

int main()
{
    assert(opennow::audio::InitialJitterHoldUs(30) <
           opennow::audio::InitialJitterHoldUs(100));
    assert(opennow::audio::ResyncJitterHoldUs(100) == 50000);
    return 0;
}
