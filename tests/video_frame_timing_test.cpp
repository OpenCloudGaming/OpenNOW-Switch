#include "../app/src/stream/ffmpeg/VideoFrameTiming.hpp"

#include <cassert>

int main()
{
    using namespace opennow::video;

    assert(DecideFrame(90000, false, 0, true, 90000) == FrameDecision::Present);
    assert(DecideFrame(91500, false, 0, true, 90000) == FrameDecision::HoldPrevious);
    assert(DecideFrame(90000, true, 90600, true, 90000) == FrameDecision::DropSuperseded);
    assert(DecideFrame(90000, true, 91500, true, 90000) == FrameDecision::Present);
    assert(DecideFrame(91500, false, 0, false, 90000) == FrameDecision::Present);

    // RTP wraparound must retain signed timestamp ordering.
    assert(RtpDelta(0x00000100u, 0xfffffff0u) > 0);
    assert(RtpDelta(0xfffffff0u, 0x00000100u) < 0);
    return 0;
}
