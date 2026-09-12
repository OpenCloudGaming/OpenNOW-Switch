#include "stream/audio/AudioPlaybackTimeline.hpp"

#include <cassert>

int main()
{
    opennow::audio::PlaybackTimeline<3> timeline;
    assert(!timeline.position(0));
    assert(timeline.append(0, 9000, 1));
    assert(timeline.append(480, 9480, 1));
    assert(timeline.append(960, 11880, 1));
    assert(timeline.full());
    assert(!timeline.append(1440, 12360, 1));
    assert(timeline.position(959)->timestamp == 9959);
    assert(timeline.position(960)->timestamp == 11880);
    assert(timeline.position(1440)->timestamp == 12360);
    timeline.discardBefore(959);
    assert(!timeline.full());
    assert(!timeline.position(479));
    assert(timeline.position(959)->timestamp == 9959);
    assert(timeline.append(1440, 0xffffff00, 2));
    assert(timeline.position(1439)->ssrc == 1);
    assert(timeline.position(1440)->ssrc == 2);
    assert(timeline.position(1920)->timestamp == 224);
    timeline.discardBefore(1920);
    assert(!timeline.full());
    assert(timeline.position(1920)->timestamp == 224);
    timeline.clear();
    assert(!timeline.position(1920));
}
