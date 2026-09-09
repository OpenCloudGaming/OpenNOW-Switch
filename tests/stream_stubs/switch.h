#pragma once

#include <cstddef>
#include <cstdint>

using Result = std::uint32_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;

constexpr bool R_FAILED(Result result) { return result != 0; }
constexpr bool R_SUCCEEDED(Result result) { return result == 0; }

struct HwopusDecoder { int generation = 0; };
struct HwopusHeader { std::uint32_t size; std::uint32_t final_range; };
enum PcmFormat { PcmFormat_Int16 };
struct AudioOutBuffer {
    AudioOutBuffer* next;
    void* buffer;
    u64 buffer_size;
    u64 data_size;
    u64 data_offset;
};

Result hwopusDecoderInitialize(HwopusDecoder*, int, int);
void hwopusDecoderExit(HwopusDecoder*);
Result hwopusDecodeInterleaved(HwopusDecoder*, std::int32_t*, std::int32_t*,
                              const void*, std::size_t, void*, std::size_t);
Result audoutInitialize();
void audoutExit();
u32 audoutGetSampleRate();
u32 audoutGetChannelCount();
PcmFormat audoutGetPcmFormat();
Result audoutGetAudioOutPlayedSampleCount(u64*);
Result audoutGetReleasedAudioOutBuffer(AudioOutBuffer**, u32*);
Result audoutStartAudioOut();
Result audoutStopAudioOut();
Result audoutFlushAudioOutBuffers(bool*);
Result audoutAppendAudioOutBuffer(AudioOutBuffer*);
Result audoutContainsAudioOutBuffer(AudioOutBuffer*, bool*);
void armDCacheFlush(void*, std::size_t);
