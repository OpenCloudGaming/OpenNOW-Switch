#include "video_quality_policy.hpp"

#include <cassert>

int main()
{
    const auto original = opennow::video::ResolveQualityTuning("Original");
    const auto adaptive = opennow::video::ResolveQualityTuning("Adaptive");
    const auto clarity = opennow::video::ResolveQualityTuning("Clarity");

    assert(original.initial_bitrate_percent == 70);
    assert(original.denoise_strength == 0.0f);
    assert(adaptive.initial_bitrate_percent > original.initial_bitrate_percent);
    assert(adaptive.fec_repair_percent == 8);
    assert(adaptive.sharpen_strength > 0.0f);
    assert(clarity.initial_bitrate_percent >= adaptive.initial_bitrate_percent);
    assert(clarity.preserve_reference_deblocking);
    assert(opennow::video::NextQualityMode("Adaptive") == "Clarity");
    assert(opennow::video::NextQualityMode("Clarity") == "Original");
    assert(opennow::video::NextQualityMode("invalid") == "Adaptive");
    return 0;
}
