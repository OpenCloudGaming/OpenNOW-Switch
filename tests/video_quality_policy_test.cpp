#include "video_quality_policy.hpp"

#include <cassert>

int main()
{
    const auto original = opennow::video::ResolveQualityTuning("Original");
    const auto adaptive = opennow::video::ResolveQualityTuning("Adaptive");
    const auto clarity = opennow::video::ResolveQualityTuning("Clarity");

    assert(original.denoise_strength == 0.0f);
    assert(adaptive.fec_repair_percent == 8);
    assert(adaptive.sharpen_strength > 0.0f);
    assert(clarity.preserve_reference_deblocking);

    const auto high_bitrate = opennow::video::ResolveBitrateTuning(75000);
    assert(high_bitrate.minimum_kbps == 4000);
    assert(high_bitrate.initial_kbps == 18750);
    assert(high_bitrate.maximum_kbps == 75000);
    assert(opennow::video::ResolveBitrateTuning(100000).maximum_kbps == 75000);
    assert(opennow::video::ResolveBitrateTuning(1000).maximum_kbps == 8000);

    assert(opennow::video::NextQualityMode("Adaptive") == "Clarity");
    assert(opennow::video::NextQualityMode("Clarity") == "Original");
    assert(opennow::video::NextQualityMode("invalid") == "Adaptive");
    return 0;
}
