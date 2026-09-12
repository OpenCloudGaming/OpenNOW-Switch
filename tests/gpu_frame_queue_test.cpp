#include "stream/deko3d/GpuFrameQueue.hpp"

#include <cassert>
#include <cstdlib>

static void released(void* opaque, uint8_t* data)
{
    ++*static_cast<int*>(opaque);
    std::free(data);
}

static AVFrame* make_frame(int& released_count)
{
    AVFrame* frame = av_frame_alloc();
    assert(frame);
    frame->buf[0] = av_buffer_create(static_cast<uint8_t*>(std::malloc(1)), 1,
                                   released, &released_count, 0);
    assert(frame->buf[0]);
    frame->data[0] = frame->buf[0]->data;
    return frame;
}

int main()
{
    int released_count = 0;
    opennow::video::GpuFrameQueue<unsigned, 2> frames;
    assert(!frames.retain(nullptr));
    AVFrame* first = make_frame(released_count);
    assert(frames.retain(first));
    av_frame_free(&first);
    frames.setCompletion(1);
    assert(released_count == 0);

    AVFrame* second = make_frame(released_count);
    assert(frames.retain(second));
    av_frame_free(&second);
    frames.setCompletion(2);
    frames.releaseCompleted([](unsigned) { return false; });
    assert(frames.size() == 2 && released_count == 0);

    AVFrame* third = make_frame(released_count);
    assert(!frames.retain(third));
    frames.releaseCompleted([](unsigned fence) { return fence <= 1; });
    assert(frames.size() == 1 && released_count == 1);
    frames.releaseCompleted([](unsigned) { return true; });
    assert(frames.size() == 1 && released_count == 1);
    frames.setCompletion(3);
    assert(frames.retain(third));
    av_frame_free(&third);
    frames.setCompletion(4);
    frames.releaseCompleted([](unsigned fence) { return fence <= 2; });
    assert(frames.size() == 2 && released_count == 1);
    frames.discardLast();
    assert(frames.size() == 1 && released_count == 2);
    frames.clear();
    assert(frames.size() == 0 && released_count == 3);
}
