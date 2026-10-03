#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#define private public
#include "webrtc_session.hpp"
#undef private
#include "stream/ffmpeg/AVFrameHolder.hpp"

#define main existing_audio_pipeline_test_main
#include "audio_pipeline_test.cpp"
#undef main

extern "C" {
int peer_init() { return 0; }
void peer_deinit() {}
}

WebRtcSession::WebRtcSession(const std::string&, const std::string&, const std::string&,
                             const std::string&, int, const std::vector<opennow::IceServerInfo>&,
                             const opennow::StreamSettings&) {}
WebRtcSession::~WebRtcSession() = default;
SignalingClient::~SignalingClient() = default;
WebSocketClient::~WebSocketClient() = default;
FFmpegVideoDecoder::~FFmpegVideoDecoder() = default;
int FFmpegVideoDecoder::setup(int, int, int, int, void*, int) { return 0; }
void FFmpegVideoDecoder::cleanup() {}
int FFmpegVideoDecoder::submit_decode_unit(uint8_t*, int, int64_t) { return 0; }
int FFmpegVideoDecoder::capabilities() const { return 0; }
VideoDecodeStats* FFmpegVideoDecoder::video_decode_stats() { return nullptr; }

namespace {

template <typename Predicate>
void wait_until(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void send_audio(AudioPipeline& pipeline, uint32_t ssrc, uint16_t sequence, uint32_t timestamp)
{
    uint8_t payload[] = {0xf0, static_cast<uint8_t>(sequence)};
    PeerAudioPacket packet {};
    packet.data = payload;
    packet.size = sizeof(payload);
    packet.timestamp = timestamp;
    packet.ssrc = ssrc;
    packet.sequence = sequence;
    packet.payload_type = 111;
    pipeline.submit(packet);
}

void push_video(AVFrameQueue& frames, int64_t pts)
{
    AVFrame* frame = av_frame_alloc();
    assert(frame);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 16;
    frame->height = 16;
    frame->pts = pts;
    assert(av_frame_get_buffer(frame, 0) == 0);
    frames.push(frame);
    av_frame_free(&frame);
    assert(frames.size() <= 3);
}

void check_video_progress(WebRtcSession& session, AVFrameQueue& frames, int64_t& pts)
{
    int presented = 0;
    const size_t holds_before = frames.getTimingHoldStat();
    const size_t drops_before = frames.getOverflowDropStat();
    for (int i = 0; i < 180; ++i) {
        pts += 1500;
        push_video(frames, pts);
        bool reused = false;
        uint64_t generation = 0;
        const auto* frame = frames.pop(reused, generation, session.video_target_rtp_timestamp());
        assert(frame);
        if (!reused) {
            assert(frame->pts == pts);
            ++presented;
        }
    }
    std::printf("Audio unavailable: new_video_frames=180 presented=%d held=%zu overflow_drops=%zu\n",
        presented, frames.getTimingHoldStat() - holds_before,
        frames.getOverflowDropStat() - drops_before);
    std::fflush(stdout);
    assert(presented == 180);
    assert(frames.getTimingHoldStat() == holds_before);
    assert(frames.getOverflowDropStat() == drops_before);
    assert(session.video_target_rtp_timestamp() == AV_NOPTS_VALUE);
}

void drain_audio(AudioPipeline& pipeline, int underrun)
{
    {
        std::lock_guard lock(mutex);
        assert(output.size() <= 5);
        release_output = true;
    }
    await([underrun] { return stops == underrun; });
    wait_until([&] { return !pipeline.is_playing(); });
    {
        std::lock_guard lock(mutex);
        assert(output.empty());
        release_output = false;
    }
}

}

int main()
{
    WebRtcSession session("", "", "", "", 0, {}, {});
    session.audio_ = std::make_unique<AudioPipeline>();
    auto& audio = *session.audio_;
    audio.configure(1200, 30);
    session.have_video_sender_report_ = true;
    session.video_sr_ntp_us_ = 1000000;
    session.video_sr_rtp_timestamp_ = 90000;
    assert(session.video_target_rtp_timestamp() == AV_NOPTS_VALUE);
    assert(audio.start());
    send_audio(audio, 1, 0, 0);
    send_audio(audio, 1, 1, 480);
    audio.set_sender_report(1, 1000000, 0);
    assert(session.video_target_rtp_timestamp() == AV_NOPTS_VALUE);
    {
        std::lock_guard lock(mutex);
        assert(starts == 0);
    }
    send_audio(audio, 1, 2, 960);
    await([] { return starts == 1; });
    wait_until([&] { return session.video_target_rtp_timestamp() != AV_NOPTS_VALUE; });

    AVFrameQueue frames;
    int64_t pts = 92700;
    push_video(frames, pts);
    bool reused = false;
    uint64_t generation = 0;
    assert(frames.pop(reused, generation)->pts == pts && !reused);
    drain_audio(audio, 1);
    assert(audio.playback_ntp_us() == 1030000);
    check_video_progress(session, frames, pts);

    send_audio(audio, 1, 3, 145440);
    send_audio(audio, 1, 4, 145920);
    await([] { return output.size() == 2; });
    {
        std::lock_guard lock(mutex);
        assert(starts == 1);
    }
    assert(session.video_target_rtp_timestamp() == AV_NOPTS_VALUE);
    send_audio(audio, 1, 5, 146400);
    await([] { return starts == 2; });
    wait_until([&] { return session.video_target_rtp_timestamp() != AV_NOPTS_VALUE; });
    const auto resumed_target = session.video_target_rtp_timestamp();
    assert(resumed_target >= pts && resumed_target <= pts + 2700);
    push_video(frames, pts + 90000);
    assert(frames.pop(reused, generation, resumed_target)->pts == pts && reused);
    assert(frames.size() == 1);
    frames.cleanup();
    push_video(frames, pts);
    assert(frames.pop(reused, generation)->pts == pts && !reused);

    send_audio(audio, 2, 0, 0);
    send_audio(audio, 2, 1, 480);
    audio.set_sender_report(2, 6000000, 0);
    assert(session.video_target_rtp_timestamp() == AV_NOPTS_VALUE);
    check_video_progress(session, frames, pts);
    send_audio(audio, 2, 2, 960);
    wait_until([] {
        std::lock_guard lock(mutex);
        return decoded_sequences.size() == 9 && reclaim_queries >= 1;
    });
    {
        std::lock_guard lock(mutex);
        assert(output.size() == 5);
    }
    assert(session.video_target_rtp_timestamp() == AV_NOPTS_VALUE);
    drain_audio(audio, 2);
    assert(audio.playback_ntp_us() == 6020000);
    check_video_progress(session, frames, pts);

    send_audio(audio, 2, 3, 146880);
    send_audio(audio, 2, 4, 147360);
    await([] { return output.size() == 2; });
    assert(session.video_target_rtp_timestamp() == AV_NOPTS_VALUE);
    send_audio(audio, 2, 5, 147840);
    await([] { return starts == 3; });
    wait_until([&] { return session.video_target_rtp_timestamp() != AV_NOPTS_VALUE; });
    assert(audio.playback_ntp_us() >= 9060000);
    audio.stop();
    assert(session.video_target_rtp_timestamp() == AV_NOPTS_VALUE);
    check_video_progress(session, frames, pts);
    std::puts("Audio underrun, video progress and synchronized resume: PASS");
}
