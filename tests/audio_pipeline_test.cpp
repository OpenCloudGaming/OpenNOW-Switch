#include "stream/audio/AudioPipeline.hpp"
#include "peer_connection.h"
#include <switch.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {
std::mutex mutex;
std::condition_variable changed;
std::deque<AudioOutBuffer*> output;
std::vector<int> decode_generations;
std::vector<int> decoded_sequences;
std::vector<int> reclaim_queries_at_append;
int decoder_generation = 0;
int reclaim_queries = 0;
int starts = 0;
int stops = 0;
u64 played_samples = 0;
bool release_output = false;

template <typename Predicate>
void await(Predicate predicate)
{
    std::unique_lock lock(mutex);
    assert(changed.wait_for(lock, std::chrono::seconds(2), predicate));
}

void submit_epoch(AudioPipeline& pipeline, uint32_t ssrc)
{
    uint8_t payload[] = {0xf0, 0x00};
    for (uint16_t sequence : {2, 0, 0, 1}) {
        payload[1] = static_cast<uint8_t>(sequence);
        PeerAudioPacket packet {};
        packet.data = payload;
        packet.size = sizeof(payload);
        packet.timestamp = 480 * sequence;
        packet.ssrc = ssrc;
        packet.sequence = sequence;
        packet.payload_type = 111;
        pipeline.submit(packet);
    }
}
}

namespace opennow {
bool StreamDiagnosticsEnabled() { return false; }
}

Result hwopusDecoderInitialize(HwopusDecoder* decoder, int, int)
{
    std::lock_guard lock(mutex);
    decoder->generation = ++decoder_generation;
    return 0;
}
void hwopusDecoderExit(HwopusDecoder*) {}
Result hwopusDecodeInterleaved(HwopusDecoder* decoder, int32_t* consumed, int32_t* samples,
                              const void* input, size_t size, void* pcm, size_t capacity)
{
    std::lock_guard lock(mutex);
    *consumed = static_cast<int32_t>(size);
    *samples = 480;
    assert(capacity >= 480 * 2 * sizeof(int16_t));
    std::memset(pcm, 0, 480 * 2 * sizeof(int16_t));
    decode_generations.push_back(decoder->generation);
    assert(size == sizeof(HwopusHeader) + 2);
    decoded_sequences.push_back(static_cast<const uint8_t*>(input)[sizeof(HwopusHeader) + 1]);
    reclaim_queries = 0;
    changed.notify_all();
    return 0;
}
Result audoutInitialize() { return 0; }
void audoutExit() {}
u32 audoutGetSampleRate() { return 48000; }
u32 audoutGetChannelCount() { return 2; }
PcmFormat audoutGetPcmFormat() { return PcmFormat_Int16; }
Result audoutGetAudioOutPlayedSampleCount(u64* samples)
{
    std::lock_guard lock(mutex);
    *samples = played_samples;
    return 0;
}
Result audoutGetReleasedAudioOutBuffer(AudioOutBuffer** buffer, u32* count)
{
    std::lock_guard lock(mutex);
    ++reclaim_queries;
    *buffer = nullptr;
    *count = 0;
    if (release_output && !output.empty()) {
        *buffer = output.front();
        *count = 1;
        played_samples += output.front()->data_size / (2 * sizeof(int16_t));
        output.pop_front();
    }
    return 0;
}
Result audoutStartAudioOut()
{
    std::lock_guard lock(mutex);
    ++starts;
    changed.notify_all();
    return 0;
}
Result audoutStopAudioOut()
{
    std::lock_guard lock(mutex);
    ++stops;
    changed.notify_all();
    return 0;
}
Result audoutFlushAudioOutBuffers(bool* flushed)
{
    std::lock_guard lock(mutex);
    output.clear();
    *flushed = true;
    return 0;
}
Result audoutAppendAudioOutBuffer(AudioOutBuffer* buffer)
{
    std::lock_guard lock(mutex);
    output.push_back(buffer);
    reclaim_queries_at_append.push_back(reclaim_queries);
    changed.notify_all();
    return 0;
}
Result audoutContainsAudioOutBuffer(AudioOutBuffer* buffer, bool* contains)
{
    std::lock_guard lock(mutex);
    *contains = std::find(output.begin(), output.end(), buffer) != output.end();
    return 0;
}
void armDCacheFlush(void*, size_t) {}

int main()
{
    AudioPipeline pipeline;
    pipeline.configure(1000, 30);
    assert(pipeline.start());
    submit_epoch(pipeline, 1);
    await([] { return starts == 1; });
    {
        std::lock_guard lock(mutex);
        assert(decode_generations == std::vector<int>({1, 1, 1}));
        assert(decoded_sequences == std::vector<int>({0, 1, 2}));
        assert(reclaim_queries_at_append == std::vector<int>({1, 1, 1}));
        release_output = true;
    }
    await([] { return stops == 1; });
    {
        std::lock_guard lock(mutex);
        assert(output.empty());
        release_output = false;
    }

    submit_epoch(pipeline, 2);
    await([] { return starts == 2; });
    {
        std::lock_guard lock(mutex);
        assert(decode_generations == std::vector<int>({1, 1, 1, 2, 2, 2}));
        assert(decoded_sequences == std::vector<int>({0, 1, 2, 0, 1, 2}));
        assert(output.size() == 3);
    }
    pipeline.stop();
    return 0;
}
