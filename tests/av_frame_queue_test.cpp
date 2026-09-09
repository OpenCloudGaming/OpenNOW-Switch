#include "stream/ffmpeg/AVFrameHolder.hpp"

#include <cassert>

static void push(AVFrameQueue& queue, int64_t pts)
{
    AVFrame* frame = av_frame_alloc();
    assert(frame);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 16;
    frame->height = 16;
    assert(av_frame_get_buffer(frame, 0) == 0);
    frame->pts = pts;
    queue.push(frame);
    av_frame_free(&frame);
}

int main()
{
    AVFrameQueue queue;
    bool reused = false;
    uint64_t generation = 0;
    assert(queue.pop(reused, generation) == nullptr);
    assert(reused && generation == 0);

    push(queue, 90000);
    push(queue, 91500);
    push(queue, 93000);
    assert(queue.pop(reused, generation)->pts == 93000);
    assert(!reused && generation == 3 && queue.size() == 0);
    assert(queue.getFramesDropStat() == 2);
    assert(queue.getOverflowDropStat() == 0);
    assert(queue.pop(reused, generation)->pts == 93000);
    assert(reused && generation == 3);

    for (int64_t pts = 94500; pts <= 108000; pts += 1500) {
        push(queue, pts);
        assert(queue.pop(reused, generation)->pts == pts);
        assert(!reused && queue.size() == 0);
    }

    queue.cleanup();
    push(queue, 90000);
    push(queue, 91500);
    push(queue, 93000);
    assert(queue.pop(reused, generation, 90000)->pts == 90000);
    assert(!reused && generation == 1 && queue.size() == 2);
    assert(queue.pop(reused, generation, 90000)->pts == 90000);
    assert(reused && generation == 1 && queue.size() == 2);
    assert(queue.pop(reused, generation, 93000)->pts == 93000);
    assert(!reused && generation == 3 && queue.size() == 0);
    assert(queue.getTimingDropStat() == 1);
    assert(queue.getTimingHoldStat() == 1);

    queue.cleanup();
    for (int i = 0; i < 10; ++i)
        push(queue, AV_NOPTS_VALUE);
    assert(queue.size() == 3 && queue.getOverflowDropStat() == 7);
    assert(queue.pop(reused, generation));
    assert(!reused && generation == 10 && queue.size() == 0);
    assert(queue.getFramesDropStat() == 9);
    queue.cleanup();
    assert(queue.pop(reused, generation) == nullptr);
    assert(reused && generation == 0);
    return 0;
}
