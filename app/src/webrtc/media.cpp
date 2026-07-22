#include "webrtc_session.hpp"
#include "stream/audio/AudioRtpUtils.hpp"
#include "stream/DecodeQueuePolicy.hpp"
#include "stream/ffmpeg/AVFrameHolder.hpp"
#include "internal.hpp"

#include <jansson.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

using namespace opennow::webrtc::internal;


void WebRtcSession::start_decoder_worker() {
    if (decoder_running_.exchange(true))
        return;

    decoder_thread_ = std::thread(&WebRtcSession::decoder_loop, this);
    AppendStreamLog("DECODE worker_started");
}

void WebRtcSession::decoder_loop() {
    for (;;) {
        DecodeUnit unit;
        {
            std::unique_lock<std::mutex> lock(decoder_queue_mutex_);
            decoder_queue_cv_.wait(lock, [this] {
                return !decoder_running_.load(std::memory_order_acquire) || !decoder_queue_.empty();
            });
            if (!decoder_running_.load(std::memory_order_acquire))
                break;

            unit = std::move(decoder_queue_.front());
            decoder_queue_.pop_front();
        }

        const auto decode_started_at = std::chrono::steady_clock::now();
        const uint64_t queue_wait_us = unit.enqueued_at.time_since_epoch().count() == 0 ? 0 :
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                decode_started_at - unit.enqueued_at).count());
        RecordLatency(queue_wait_buckets_, queue_wait_us);
        AtomicMax(queue_wait_us_max_, queue_wait_us);

        if (decoder_reset_requested_.exchange(false) && decoder_)
            decoder_->reset_stream();

        const int decoded = decoder_
            ? decoder_->submit_decode_unit(unit.data.data(), static_cast<int>(unit.data.size()), unit.rtp_timestamp)
            : -1;
        const uint64_t decode_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - decode_started_at).count());
        decode_us_total_.fetch_add(decode_us, std::memory_order_relaxed);
        RecordLatency(decode_latency_buckets_, decode_us);
        AtomicMax(decode_us_max_, decode_us);

        const int access_unit_count = packets_received_.load();
        if (decoded > 0) {
            last_decoded_frame_at_us_.store(NowUs(), std::memory_order_release);
            const int frame_count = frames_decoded_.fetch_add(decoded) + decoded;
            if (!first_decoded_frame_logged_) {
                first_decoded_frame_logged_ = true;
                AppendStreamLog("DECODE first_frame frames=" + std::to_string(frame_count) +
                                " accessUnits=" + std::to_string(access_unit_count));
            } else if (frame_count - last_logged_frame_count_.load(std::memory_order_relaxed) >= 120) {
                AppendStreamLog("DECODE frame_progress frames=" + std::to_string(frame_count) +
                                " accessUnits=" + std::to_string(access_unit_count));
                last_logged_frame_count_.store(frame_count, std::memory_order_relaxed);
            }
        } else if (decoded < 0) {
            const int error_count = decode_errors_.fetch_add(1) + 1;
            if (error_count <= 5 ||
                error_count - last_logged_decode_error_count_.load(std::memory_order_relaxed) >= 20) {
                AppendStreamLog("DECODE error count=" + std::to_string(error_count) +
                                " accessUnit=" + std::to_string(access_unit_count) +
                                " size=" + std::to_string(unit.data.size()) +
                                " idr=" + std::to_string(unit.idr ? 1 : 0) +
                                " preview=" + HexPreview(unit.data.data(), unit.data.size()));
                last_logged_decode_error_count_.store(error_count, std::memory_order_relaxed);
            }
            decoder_resync_required_.store(true);
            if (!decoder_ || !decoder_->uses_hardware_frames())
                decoder_reset_requested_.store(true);
            keyframe_needed_.store(true);
        }

        if (unit.idr && decoded >= 0) {
            decoder_resync_required_.store(false);
            keyframe_needed_.store(false);
        }

        {
            std::lock_guard<std::mutex> queue_lock(decoder_queue_mutex_);
            recycle_decode_buffer_locked(std::move(unit.data));
        }
    }

    AppendStreamLog("DECODE worker_stopped");
}

void WebRtcSession::recycle_decode_buffer_locked(std::vector<uint8_t>&& buffer) {
    constexpr size_t kPoolLimit = 8;
    constexpr size_t kMaximumReusableCapacity = 2 * 1024 * 1024;
    if (decoder_buffer_pool_.size() >= kPoolLimit ||
        buffer.capacity() > kMaximumReusableCapacity) {
        return;
    }
    buffer.clear();
    decoder_buffer_pool_.push_back(std::move(buffer));
}

void WebRtcSession::clear_decoder_queue_locked() {
    while (!decoder_queue_.empty()) {
        recycle_decode_buffer_locked(std::move(decoder_queue_.front().data));
        decoder_queue_.pop_front();
    }
}

void WebRtcSession::enqueue_decode_unit(const uint8_t* data, size_t size, uint32_t rtp_timestamp) {
    if (!data || size == 0 || !decoder_running_.load(std::memory_order_acquire))
        return;

    const bool idr = ContainsH264Idr(data, size);
    std::lock_guard<std::mutex> lock(decoder_queue_mutex_);

    if (decoder_resync_required_.load() && !idr) {
        decoder_queue_drops_++;
        return;
    }

    // Keep the queue bounded to about 67 ms. Crossing the bound means the
    // decoder is no longer at the live edge, so clear the dependent chain and
    // resume at an IDR instead of accumulating visible input latency.
    const size_t kMaxQueuedAccessUnits =
        opennow::video::MaximumQueuedAccessUnits(settings_.fps);
    if (decoder_queue_.size() >= kMaxQueuedAccessUnits) {
        decoder_queue_drops_ += static_cast<int>(decoder_queue_.size());
        clear_decoder_queue_locked();
        decoder_resync_required_.store(true);
        if (!decoder_ || !decoder_->uses_hardware_frames())
            decoder_reset_requested_.store(true);
        keyframe_needed_.store(true);
        if (!idr) {
            decoder_queue_drops_++;
            return;
        }
    }

    std::vector<uint8_t> buffer;
    if (!decoder_buffer_pool_.empty()) {
        buffer = std::move(decoder_buffer_pool_.front());
        decoder_buffer_pool_.pop_front();
        decoder_buffer_reuses_.fetch_add(1, std::memory_order_relaxed);
    } else {
        decoder_buffer_allocations_.fetch_add(1, std::memory_order_relaxed);
    }
    buffer.resize(size);
    std::memcpy(buffer.data(), data, size);
    decoder_queue_.push_back({
        std::move(buffer), idr, rtp_timestamp,
        std::chrono::steady_clock::now()
    });
    AtomicMax(decoder_queue_high_water_, decoder_queue_.size());
    decoder_queue_cv_.notify_one();
}


void WebRtcSession::draw(NVGcontext* vg, int width, int height, AVFrame* frame, uint64_t generation) {
    if (renderer_ && frame) {
        rendered_video_width_.store(frame->width, std::memory_order_relaxed);
        rendered_video_height_.store(frame->height, std::memory_order_relaxed);
        const auto render_started_at = std::chrono::steady_clock::now();
        last_rendered_video_pts_.store(frame->pts);
        renderer_->drawLatest(vg, width, height, frame, VIDEO_FORMAT_H264, generation);
        const uint64_t render_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - render_started_at).count());
        render_us_total_.fetch_add(render_us, std::memory_order_relaxed);
        RecordLatency(render_latency_buckets_, render_us);
        AtomicMax(render_us_max_, render_us);
        const uint64_t previous_generation = last_presented_generation_.exchange(generation);
        if (generation != 0 && generation != previous_generation)
            presented_frames_.fetch_add(1, std::memory_order_relaxed);
    }
}


void WebRtcSession::maybe_request_startup_keyframe_retry() {
    if (!pc_ || !peer_completed_seen_ || packets_received_.load() > 0)
        return;

    const PeerConnectionState state = peer_connection_get_state(pc_);
    if (state != PEER_CONNECTION_COMPLETED)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (last_startup_keyframe_request_.time_since_epoch().count() != 0 &&
        now - last_startup_keyframe_request_ < std::chrono::seconds(3)) {
        return;
    }

    if (keyframe_request_attempts_ >= 5)
        return;

    last_startup_keyframe_request_ = now;
    request_keyframe("startup_no_video");
}

void WebRtcSession::maybe_recover_decode_stall() {
    const uint64_t last_decoded_us = last_decoded_frame_at_us_.load(std::memory_order_acquire);
    if (!pc_ || frames_decoded_.load() == 0 || last_decoded_us == 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const uint64_t now_us = NowUs();
    const uint64_t stalled_ms = now_us >= last_decoded_us ? (now_us - last_decoded_us) / 1000 : 0;
    if (stalled_ms < 1500)
        return;

    if (last_decode_stall_request_at_.time_since_epoch().count() != 0 &&
        now - last_decode_stall_request_at_ < std::chrono::seconds(2)) {
        return;
    }

    // A completed connection can still stop producing complete H.264 pictures.
    // Ask for a fresh IDR promptly, but never flood the signaling channel.
    if (keyframe_request_attempts_ >= 12)
        return;

    last_decode_stall_request_at_ = now;
    AppendStreamLog("DECODE timeoutMs=" + std::to_string(stalled_ms) +
                    " packets=" + std::to_string(packets_received_.load()) +
                    " frames=" + std::to_string(frames_decoded_.load()));
    {
        std::lock_guard<std::mutex> queue_lock(decoder_queue_mutex_);
        decoder_queue_drops_ += static_cast<int>(decoder_queue_.size());
        clear_decoder_queue_locked();
    }
    decoder_resync_required_.store(true);
    if (!decoder_ || !decoder_->uses_hardware_frames())
        decoder_reset_requested_.store(true);
    request_keyframe("decode_timeout");
}

void WebRtcSession::maybe_recover_rtp_damage() {
    if (!pc_)
        return;
    PeerVideoRtpStats stats {};
    if (peer_connection_get_video_rtp_stats(pc_, &stats) != 0 ||
        stats.access_units_dropped <= last_rtp_access_units_dropped_) {
        return;
    }

    const uint32_t newly_dropped = stats.access_units_dropped - last_rtp_access_units_dropped_;
    last_rtp_access_units_dropped_ = stats.access_units_dropped;
    const auto now = std::chrono::steady_clock::now();
    if (last_rtp_damage_request_at_.time_since_epoch().count() == 0 ||
        now - last_rtp_damage_request_at_ >= std::chrono::seconds(1)) {
        last_rtp_damage_request_at_ = now;
        AppendStreamLog("RTP damage auDroppedNew/total=" + std::to_string(newly_dropped) +
                        "/" + std::to_string(stats.access_units_dropped) +
                        " gaps=" + std::to_string(stats.sequence_gaps) +
                        " reordered=" + std::to_string(stats.reordered_packets) +
                        " late=" + std::to_string(stats.late_packets_dropped) +
                        " forced=" + std::to_string(stats.forced_sequence_skips));
    }
    // Once a reference picture is missing, subsequent P frames are not safe to
    // display on either backend. Hold the previous good frame and resume from
    // an IDR; only the software decoder is flushed because Deko3D can still own
    // references to NVDEC output surfaces.
    if (!decoder_resync_required_.exchange(true)) {
        std::lock_guard<std::mutex> queue_lock(decoder_queue_mutex_);
        decoder_queue_drops_ += static_cast<int>(decoder_queue_.size());
        clear_decoder_queue_locked();
        if (!decoder_ || !decoder_->uses_hardware_frames())
            decoder_reset_requested_.store(true);
        keyframe_needed_.store(true);
    }
    // Do not flush healthy queued frames or request an IDR for every damaged
    // access unit. Large IDRs amplify packet bursts and previously created a
    // self-sustaining PLI/loss loop. maybe_recover_decode_stall() remains the
    // authoritative recovery path when output actually stops.
}


void WebRtcSession::request_keyframe(const char* reason) {
    const auto now = std::chrono::steady_clock::now();
    constexpr auto kMinimumPliInterval = std::chrono::milliseconds(1500);
    if (last_keyframe_request_at_.time_since_epoch().count() != 0 &&
        now - last_keyframe_request_at_ < kMinimumPliInterval) {
        // Keep one coalesced request pending. poll() will retry it after the
        // cooldown instead of flooding the server with expensive IDR frames.
        if (decoder_resync_required_.load())
            keyframe_needed_.store(true);
        return;
    }

    last_keyframe_request_at_ = now;
    keyframe_request_attempts_++;
    const int pli_result = pc_ ? peer_connection_request_video_keyframe(pc_) : -1;
    AppendStreamLog("KEYFRAME request reason=" + std::string(reason ? reason : "switch_stream") +
                    " attempt=" + std::to_string(keyframe_request_attempts_) +
                    " rtcpPli=" + std::to_string(pli_result) +
                    " packets=" + std::to_string(packets_received_.load()) +
                    " frames=" + std::to_string(frames_decoded_.load()) +
                    " backlog=" + std::to_string(std::max(0, packets_received_.load() - frames_decoded_.load())));

    if (pli_result >= 0 || !signaling_client_ || remote_peer_id_ <= 0)
        return;

    // Keep the legacy signaling request as a fallback until the video SSRC is known.
    json_t* req = json_object();
    json_object_set_new(req, "type", json_string("request_keyframe"));
    json_object_set_new(req, "reason", json_string(reason ? reason : "switch_stream"));
    json_object_set_new(req, "backlogFrames", json_integer(std::max(0, packets_received_.load() - frames_decoded_.load())));
    json_object_set_new(req, "attempt", json_integer(keyframe_request_attempts_));
    send_peer_payload(req);
    json_decref(req);
}

void WebRtcSession::on_video_packet(const PeerVideoPacket& packet) {
    const uint8_t* data = packet.data;
    const size_t size = packet.size;
    last_video_packet_at_us_.store(NowUs(), std::memory_order_release);
    video_access_unit_bytes_.fetch_add(size, std::memory_order_relaxed);
    AtomicMax(video_access_unit_max_bytes_, static_cast<uint64_t>(size));
    video_ssrc_ = packet.ssrc;
    for (const auto& report : sender_reports_) {
        if (report.ssrc == video_ssrc_) {
            video_sr_ntp_us_ = report.ntp_us;
            video_sr_rtp_timestamp_ = report.rtp_timestamp;
            have_video_sender_report_ = true;
            break;
        }
    }
    const int packet_count = packets_received_.fetch_add(1) + 1;
    if (!first_video_packet_logged_) {
        first_video_packet_logged_ = true;
        AppendStreamLog("VIDEO first_access_unit size=" + std::to_string(size) +
                        " idr=" + std::to_string(ContainsH264Idr(data, size) ? 1 : 0) +
                        " preview=" + HexPreview(data, size));
        log_stream_summary("first_video_packet");
    } else if (packet_count - last_logged_packet_count_ >= 120) {
        AppendStreamLog("VIDEO access_unit_progress units=" + std::to_string(packet_count) +
                        " size=" + std::to_string(size) +
                        " frames=" + std::to_string(frames_decoded_.load()) +
                        " decodeErrors=" + std::to_string(decode_errors_.load()) +
                        " queueDrops=" + std::to_string(decoder_queue_drops_.load()));
        last_logged_packet_count_ = packet_count;
    }

    enqueue_decode_unit(data, size, packet.timestamp);
}

void WebRtcSession::on_audio_packet(const PeerAudioPacket& packet) {
    const bool new_audio_ssrc = audio_ssrc_ != packet.ssrc;
    audio_ssrc_ = packet.ssrc;
    if (audio_) {
        audio_->submit(packet);
        if (new_audio_ssrc) {
            for (const auto& report : sender_reports_) {
                if (report.ssrc == audio_ssrc_) {
                    audio_->set_sender_report(report.ssrc, report.ntp_us, report.rtp_timestamp);
                    break;
                }
            }
        }
    }
}

void WebRtcSession::on_rtp_sender_report(uint32_t ssrc, uint64_t ntp_us, uint32_t rtp_timestamp) {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    auto found = std::find_if(sender_reports_.begin(), sender_reports_.end(), [ssrc](const SenderReport& item) {
        return item.ssrc == ssrc;
    });
    if (found == sender_reports_.end())
        sender_reports_.push_back({ssrc, ntp_us, rtp_timestamp});
    else
        *found = {ssrc, ntp_us, rtp_timestamp};

    if (ssrc == audio_ssrc_ && audio_)
        audio_->set_sender_report(ssrc, ntp_us, rtp_timestamp);
    if (ssrc == video_ssrc_) {
        video_sr_ntp_us_ = ntp_us;
        video_sr_rtp_timestamp_ = rtp_timestamp;
        have_video_sender_report_ = true;
    }
}

int64_t WebRtcSession::video_target_rtp_timestamp() const {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    if (!audio_ || !have_video_sender_report_)
        return AV_NOPTS_VALUE;
    const int64_t audio_ntp = audio_->playback_ntp_us();
    if (audio_ntp < 0)
        return AV_NOPTS_VALUE;
    return opennow::audio::RtpTimestampAtNtp(
        video_sr_rtp_timestamp_, static_cast<int64_t>(video_sr_ntp_us_), audio_ntp, 90000);
}
