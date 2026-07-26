#include "stream/StreamDiagnosticsPolicy.hpp"

#include <cassert>
#include <string_view>

using opennow::diagnostics::DeliveredKbps;
using opennow::diagnostics::FrameGapUs;
using opennow::diagnostics::FramePathLabel;
using opennow::diagnostics::PerSecondRate;

static_assert(PerSecondRate(200, 100, 1.0) == 100.0);
static_assert(PerSecondRate(100, 100, 1.0) == 0.0);
static_assert(PerSecondRate(200, 100, 0.0) == 0.0);
static_assert(PerSecondRate(50, 100, 1.0) == 0.0);

static_assert(DeliveredKbps(125000, 0, 1.0) == 1000.0);

static_assert(FrameGapUs(0, 12345) == 0);
static_assert(FrameGapUs(1000, 1500) == 500);
static_assert(FrameGapUs(1500, 1000) == 0);

int main()
{
    // A counter that goes backwards (snapshot race, or a session restart
    // that resets an underlying atomic) must never produce a negative or
    // wrapped rate.
    assert(PerSecondRate(0, 500, 1.0) == 0.0);

    // Bitrate scales linearly with elapsed time for a fixed byte delta.
    assert(DeliveredKbps(250000, 0, 2.0) == 1000.0);

    // Frame path label is a fixed two-value vocabulary.
    assert(FramePathLabel(true) == std::string_view("hw"));
    assert(FramePathLabel(false) == std::string_view("sw"));

    // First presented frame in a session has nothing to diff against.
    assert(FrameGapUs(0, 999) == 0);
    // A large real gap (e.g. a stutter) is reported in full.
    assert(FrameGapUs(1'000'000, 1'250'000) == 250'000);

    return 0;
}
