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
#include "webrtc/internal.hpp"
#include "webrtc/startup_timeout_policy.hpp"
#include "stream/ffmpeg/AVFrameHolder.hpp"

#include <cassert>
#include <cstdio>

namespace {
std::deque<int> send_results;
std::deque<int> receive_results;
WebRtcSession* active_session = nullptr;
int64_t next_pts = 90000;
}

extern "C" {
int peer_init() { return 0; }
void peer_deinit() {}

int __wrap_avcodec_send_packet(AVCodecContext*, const AVPacket*)
{
    assert(!send_results.empty());
    const int result = send_results.front();
    send_results.pop_front();
    if (send_results.empty() && receive_results.empty())
        active_session->decoder_running_.store(false);
    return result;
}

int __wrap_avcodec_receive_frame(AVCodecContext*, AVFrame* frame)
{
    assert(!receive_results.empty());
    const int result = receive_results.front();
    receive_results.pop_front();
    if (send_results.empty() && receive_results.empty())
        active_session->decoder_running_.store(false);
    if (result < 0)
        return result;
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = 16;
    frame->height = 16;
    frame->pts = next_pts;
    next_pts += 1500;
    assert(av_frame_get_buffer(frame, 0) == 0);
    return 0;
}
}

namespace opennow::webrtc::internal {
void AppendStreamLog(const std::string&) {}
}

WebRtcSession::WebRtcSession(const std::string&, const std::string&, const std::string&,
                             const std::string&, int, const std::vector<opennow::IceServerInfo>&,
                             const opennow::StreamSettings&) {}
WebRtcSession::~WebRtcSession()
{
    if (decoder_)
        decoder_->cleanup();
}
SignalingClient::~SignalingClient() = default;
WebSocketClient::~WebSocketClient() = default;
struct AudioPipeline::Impl {};
AudioPipeline::~AudioPipeline() = default;

static void run_worker(WebRtcSession& session, std::deque<int> sends,
                       std::deque<int> receives, bool queue_following_frame)
{
    send_results = std::move(sends);
    receive_results = std::move(receives);
    active_session = &session;
    session.decoder_running_.store(true);
    const uint8_t idr[] = {0, 0, 0, 1, 0x65};
    session.enqueue_decode_unit(idr, sizeof(idr), 90000);
    if (queue_following_frame)
        session.enqueue_decode_unit(idr, sizeof(idr), 91500);
    session.decoder_loop();
    assert(send_results.empty() && receive_results.empty());
}

static void check_partial_output(std::deque<int> sends, std::deque<int> receives)
{
    WebRtcSession session("", "", "", "", 0, {}, {});
    session.decoder_ = std::make_unique<FFmpegVideoDecoder>("Adaptive");
    assert(session.decoder_->setup(VIDEO_FORMAT_H264, 16, 16, 60, nullptr,
                                    VIDEO_DECODER_FORCE_SOFTWARE) == 0);
    run_worker(session, std::move(sends), std::move(receives), true);
    bool published = false;
    AVFrameHolder::instance().get([&](AVFrame* frame, uint64_t generation, bool reused) {
        assert(frame->buf[0] && generation == 1 && !reused);
        published = true;
    });
    assert(published);
    assert(session.frames_decoded_.load() == 1);
    assert(opennow::webrtc::DetectStartupTimeout(
        true, session.frames_decoded_.load() > 0, std::chrono::seconds(15),
        std::chrono::seconds(15)) == opennow::webrtc::StartupTimeout::None);
    assert(session.last_decoded_frame_at_us_.load() > 0);
    assert(session.decode_errors_.load() == 1);
    assert(session.decoder_recovery_.waiting_for_idr());
    assert(session.decoder_recovery_.take_keyframe_request());
    assert(session.decoder_queue_.empty() && session.decoder_queue_drops_.load() == 1);

    run_worker(session, {0}, {AVERROR(EAGAIN)}, false);
    assert(session.frames_decoded_.load() == 1);
    assert(session.decode_errors_.load() == 1);
    assert(!session.decoder_recovery_.waiting_for_idr());

    run_worker(session, {AVERROR(EAGAIN), 0}, {0, AVERROR(EAGAIN), 0, AVERROR(EAGAIN)}, false);
    assert(session.frames_decoded_.load() == 3);
    assert(session.decode_errors_.load() == 1);
    assert(!session.decoder_recovery_.waiting_for_idr());
}

int main()
{
    check_partial_output({0}, {0, AVERROR_INVALIDDATA});
    check_partial_output({AVERROR(EAGAIN)}, {0, AVERROR_INVALIDDATA});
    check_partial_output({AVERROR(EAGAIN), AVERROR_INVALIDDATA}, {0, AVERROR(EAGAIN)});
    std::puts("Decode output accounting and recovery: PASS");
}
