#include "stream/audio/AudioRtpUtils.hpp"

#include <cassert>
#include <cstdint>

static_assert(opennow::audio::RtpDeltaToUs(480, 0, 48000) == 10000);
static_assert(opennow::audio::RtpDeltaToUs(0x20u, 0xfffffff0u, 48000) == 1000);
static_assert(opennow::audio::AudioTimelineBase(9600, 1920) == 7680);
static_assert(opennow::audio::RtpTimestampAtNtp(9000, 1000000, 1100000, 90000) == 18000);
static_assert(opennow::audio::RtpTimestampAtNtp(0xfffffff0u, 1000000, 1001000, 48000) == 0x20u);
static_assert(!opennow::audio::ShouldResetEpoch(1000000, 1100000, true, 100, 103, 12));
static_assert(opennow::audio::ShouldResetEpoch(1000000, 1600000, true, 100, 101, 12));

int main() {
    using namespace opennow::audio;

    const uint8_t opus[] = {0x11, 0x22, 0x33};
    auto plain = ParseRedPrimary(opus, sizeof(opus), 111);
    assert(plain.data == opus && plain.size == 3 && !plain.red);

    // One 2-byte redundant block, followed by the primary Opus block.
    const uint8_t red[] = {0x80 | 111, 0x00, 0x00, 0x02, 111, 0xaa, 0xbb, 0x11, 0x22, 0x33};
    auto primary = ParseRedPrimary(red, sizeof(red), 63);
    assert(primary.red && primary.size == 3);
    assert(primary.data[0] == 0x11 && primary.data[2] == 0x33);
    auto redundant = ParseFirstRedundant(red, sizeof(red));
    assert(redundant.size == 2 && redundant.timestamp_offset == 0);
    assert(redundant.data[0] == 0xaa && redundant.data[1] == 0xbb);

    const uint8_t malformed[] = {0x80 | 111, 0x00};
    assert(ParseRedPrimary(malformed, sizeof(malformed), 63).data == nullptr);

    assert(RtpDeltaToUs(480, 0, 48000) == 10000);
    assert(RtpDeltaToUs(9000, 0, 90000) == 100000);
    assert(RtpDeltaToUs(0x00000020u, 0xfffffff0u, 48000) == 1000);
    assert(RtpDeltaToUs(0xfffffff0u, 0x00000020u, 48000) == -1000);
    assert(AudioTimelineBase(9600, 1920) == 7680);
    assert(AudioTimelineBase(100, 480) == static_cast<uint32_t>(-380));
    assert(RtpTimestampAtNtp(9000, 1000000, 1100000, 90000) == 18000);
    assert(RtpTimestampAtNtp(9000, 1000000, 900000, 90000) == 0);
    assert(RtpTimestampAtNtp(0xfffffff0u, 1000000, 1001000, 48000) == 0x20u);
    assert(!ShouldResetEpoch(1000000, 1100000, true, 100, 103, 12));
    assert(ShouldResetEpoch(1000000, 1600000, true, 100, 101, 12));
    assert(ShouldResetEpoch(1000000, 1100000, true, 100, 180, 12));
    assert(!ShouldResetEpoch(1000000, 1100000, true, 65530, 2, 12));
    return 0;
}
