#include "webrtc_session.hpp"
#include "stream/ffmpeg/AVFrameHolder.hpp"
#include "stream_diagnostics.hpp"
#include "internal.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>

#ifdef __SWITCH__
#include <sys/stat.h>
#endif

using namespace opennow::webrtc::internal;


namespace
{

std::mutex& StreamLogMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::chrono::steady_clock::time_point& InputLogStartTime()
{
    static auto start = std::chrono::steady_clock::now();
    return start;
}

} // namespace

namespace opennow::webrtc::internal
{

void AppendInputLog(const std::string& line)
{
    if (!opennow::StreamDiagnosticsEnabled())
        return;
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
#endif
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - InputLogStartTime()).count();
    std::lock_guard<std::mutex> lock(StreamLogMutex());
    std::ofstream stream("sdmc:/switch/SwitchNOW/input.log", std::ios::app);
    if (stream.is_open())
        stream << "[+" << elapsed_ms << "ms] " << line << '\n';
}

void AppendStreamLog(const std::string& line)
{
    if (!opennow::StreamDiagnosticsEnabled())
        return;
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
#endif

    static const auto log_start = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - log_start).count();

    std::lock_guard<std::mutex> lock(StreamLogMutex());

    std::ofstream stream("sdmc:/switch/SwitchNOW/signaling.log", std::ios::app);
    if (stream.is_open())
        stream << "[+" << elapsed_ms << "ms] " << line << '\n';

    std::ofstream trace("sdmc:/switch/SwitchNOW/stream_trace.log", std::ios::app);
    if (trace.is_open())
        trace << "[+" << elapsed_ms << "ms] APP " << line << '\n';
}

void ResetStreamTraceLog()
{
    if (!opennow::StreamDiagnosticsEnabled())
        return;
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
#endif
    std::lock_guard<std::mutex> lock(StreamLogMutex());
    {
        std::ofstream stream("sdmc:/switch/SwitchNOW/signaling.log", std::ios::trunc);
        if (stream.is_open()) {
            stream << "SwitchNOW signaling and media log\n";
            stream << "One file per stream attempt.\n";
            stream << "======================================\n";
        }
    }
    {
        std::ofstream stream("sdmc:/switch/SwitchNOW/stream_trace.log", std::ios::trunc);
        if (stream.is_open()) {
            stream << "SwitchNOW stream trace\n";
            stream << "One file per stream attempt. Safe to send for debugging.\n";
            stream << "=======================================================\n";
        }
    }
    {
        InputLogStartTime() = std::chrono::steady_clock::now();
        std::ofstream stream("sdmc:/switch/SwitchNOW/input.log", std::ios::trunc);
        if (stream.is_open()) {
            stream << "SwitchNOW input flight recorder\n";
            stream << "Controller samples -> Xbox encoding -> DataChannel -> SCTP result.\n";
            stream << "===============================================================\n";
        }
    }
}

void AppendTraceLog(const std::string& line)
{
    if (!opennow::StreamDiagnosticsEnabled())
        return;
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
#endif

    static const auto trace_start = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - trace_start).count();

    std::lock_guard<std::mutex> lock(StreamLogMutex());
    std::ofstream stream("sdmc:/switch/SwitchNOW/stream_trace.log", std::ios::app);
    if (stream.is_open())
        stream << "[+" << elapsed_ms << "ms] " << line << '\n';
}

void AppendTraceBlock(const std::string& title, const std::string& body)
{
    if (!opennow::StreamDiagnosticsEnabled())
        return;
    AppendTraceLog("----- " + title + " BEGIN bytes=" + std::to_string(body.size()) + " -----");
    {
        std::lock_guard<std::mutex> lock(StreamLogMutex());
        std::ofstream stream("sdmc:/switch/SwitchNOW/stream_trace.log", std::ios::app);
        if (stream.is_open())
            stream << body << (body.empty() || body.back() == '\n' ? "" : "\n");
    }
    AppendTraceLog("----- " + title + " END -----");
}

std::string PreviewText(const std::string& value, size_t max_chars)
{
    if (value.size() <= max_chars)
        return value;
    return value.substr(0, max_chars) + "...";
}

} // namespace opennow::webrtc::internal

void WebRtcSession::record_ui_event(const std::string& event) {
    AppendStreamLog("UI " + event);
}


void WebRtcSession::maybe_log_stream_diagnostics() {
    if (!settings_.debug_diagnostics)
        return;
    if (!pc_)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (last_stream_diagnostic_log_.time_since_epoch().count() != 0 &&
        now - last_stream_diagnostic_log_ < std::chrono::seconds(2)) {
        return;
    }

    const PeerConnectionState state = peer_connection_get_state(pc_);
    if (state == PEER_CONNECTION_FAILED ||
        state == PEER_CONNECTION_CLOSED ||
        state == PEER_CONNECTION_DISCONNECTED) {
        return;
    }
    if (state != PEER_CONNECTION_CHECKING &&
        state != PEER_CONNECTION_CONNECTED &&
        state != PEER_CONNECTION_COMPLETED &&
        packets_received_.load() == last_logged_packet_count_ &&
        frames_decoded_.load() == last_logged_frame_count_.load(std::memory_order_relaxed) &&
        decode_errors_.load() == last_logged_decode_error_count_.load(std::memory_order_relaxed)) {
        return;
    }

    last_stream_diagnostic_log_ = now;
    log_stream_summary("periodic");
}


void WebRtcSession::log_stream_summary(const char* reason) {
    const auto now = std::chrono::steady_clock::now();
    long long since_start_ms = -1;
    long long since_completed_ms = -1;

    if (session_started_at_.time_since_epoch().count() != 0) {
        since_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - session_started_at_).count();
    }
    if (peer_completed_at_.time_since_epoch().count() != 0) {
        since_completed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - peer_completed_at_).count();
    }

    const VideoPerformanceCounters video = get_video_performance();
    double metric_seconds = 0.0;
    if (last_video_metric_snapshot_.time_since_epoch().count() != 0) {
        metric_seconds = std::chrono::duration<double>(now - last_video_metric_snapshot_).count();
    }
    const auto rate = [metric_seconds](uint64_t current, uint64_t previous) {
        return metric_seconds > 0.0 && current >= previous
            ? static_cast<double>(current - previous) / metric_seconds
            : 0.0;
    };
    const double incoming_fps = rate(video.access_units, last_logged_video_performance_.access_units);
    const double decoded_fps = rate(video.decoded_frames, last_logged_video_performance_.decoded_frames);
    const double presented_fps = rate(video.presented_frames, last_logged_video_performance_.presented_frames);
    const double bitrate_mbps = rate(video.access_unit_bytes, last_logged_video_performance_.access_unit_bytes) * 8.0 / 1000000.0;
    last_video_metric_snapshot_ = now;
    last_logged_video_performance_ = video;

    int pair_total = 0;
    int pair_frozen = 0;
    int pair_checking = 0;
    int pair_succeeded = 0;
    int pair_failed = 0;
    PeerVideoRtpStats rtp_stats {};
    if (pc_) {
        peer_connection_get_ice_candidate_pair_stats(
            pc_, &pair_total, &pair_frozen, &pair_checking, &pair_succeeded, &pair_failed);
        (void)peer_connection_get_video_rtp_stats(pc_, &rtp_stats);
    }
    const auto& frame_holder = AVFrameHolder::instance();
    VideoRenderStats render_backend_stats {};
    if (renderer_) {
        if (const auto* stats = renderer_->video_render_stats())
            render_backend_stats = *stats;
    }

    AppendStreamLog("STREAM diag reason=" + std::string(reason ? reason : "unknown") +
                    " state=" + current_state_ +
                    " sinceStartMs=" + std::to_string(since_start_ms) +
                    " sinceCompletedMs=" + std::to_string(since_completed_ms) +
                    " signalingRx=" + std::to_string(signaling_rx_count_) +
                    " offers=" + std::to_string(offer_count_) +
                    " iceRemote=" + std::to_string(remote_ice_count_) +
                    " iceLocal=" + std::to_string(local_ice_count_) +
                    " icePairs=" + std::to_string(pair_total) +
                    "/" + std::to_string(pair_frozen) +
                    "/" + std::to_string(pair_checking) +
                    "/" + std::to_string(pair_succeeded) +
                    "/" + std::to_string(pair_failed) +
                    " hbRx=" + std::to_string(heartbeat_rx_count_) +
                    " hbTx=" + std::to_string(heartbeat_tx_count_) +
                    " dcOpen=" + std::to_string(datachannel_opened_ ? 1 : 0) +
                    " dcReq=" + std::to_string(datachannel_open_requested_ ? 1 : 0) +
                    " dcAttempts=" + std::to_string(datachannel_open_attempts_) +
                    " dcStartup=" + std::to_string(startup_control_sent_ ? 1 : 0) +
                    " rel=" + std::to_string(reliable_input_channel_requested_ ? 1 : 0) +
                    " pr=" + std::to_string(partial_input_channel_requested_ ? 1 : 0) +
                    " inputReady=" + std::to_string(input_ready_ ? 1 : 0) +
                    " inputProto=" + std::to_string(input_protocol_version_) +
                    " inputHbTx=" + std::to_string(input_heartbeat_tx_count_) +
                    " packets=" + std::to_string(packets_received_.load()) +
                    " frames=" + std::to_string(frames_decoded_.load()) +
                    " decodeErrors=" + std::to_string(decode_errors_.load()) +
                    " keyframes=" + std::to_string(keyframe_request_attempts_) +
                    " videoBackend=" + video_backend_name_ +
                    " in/dec/outFps=" + std::to_string(incoming_fps) + "/" +
                    std::to_string(decoded_fps) + "/" + std::to_string(presented_fps) +
                    " bitrateMbps=" + std::to_string(bitrate_mbps) +
                    " auBytesTotal/max=" + std::to_string(video.access_unit_bytes) + "/" +
                    std::to_string(video_access_unit_max_bytes_.load()) +
                    " decodeUsP95/max=" + std::to_string(video.decode_us_p95) + "/" +
                    std::to_string(video.decode_us_max) +
                    " queueWaitUsP95/max=" + std::to_string(video.queue_wait_us_p95) + "/" +
                    std::to_string(video.queue_wait_us_max) +
                    " renderUsP95/max=" + std::to_string(video.render_us_p95) + "/" +
                    std::to_string(video.render_us_max) +
                    " decodeQueue/current/high=" + std::to_string(video.decode_queue_size) + "/" +
                    std::to_string(video.decode_queue_high_water) +
                    " bufferPool reuse/alloc=" + std::to_string(decoder_buffer_reuses_.load()) + "/" +
                    std::to_string(decoder_buffer_allocations_.load()) +
                    " transport batches/datagrams/max=" +
                    std::to_string(transport_batches_.load()) + "/" +
                    std::to_string(transport_datagrams_.load()) + "/" +
                    std::to_string(transport_batch_high_water_.load()) +
                    " gpu retained/mappings=" +
                    std::to_string(render_backend_stats.retained_surfaces) + "/" +
                    std::to_string(render_backend_stats.surface_mappings) +
                    " rtp packets/gaps/reordered/late/forced/buffered=" +
                    std::to_string(rtp_stats.packets_received) + "/" +
                    std::to_string(rtp_stats.sequence_gaps) + "/" +
                    std::to_string(rtp_stats.reordered_packets) + "/" +
                    std::to_string(rtp_stats.late_packets_dropped) + "/" +
                    std::to_string(rtp_stats.forced_sequence_skips) + "/" +
                    std::to_string(rtp_stats.reorder_buffered_packets) +
                    " nack requests/packets=" +
                    std::to_string(rtp_stats.nack_requests) + "/" +
                    std::to_string(rtp_stats.nack_packets_requested) +
                    " rtpAu ok/drop=" + std::to_string(rtp_stats.access_units_completed) + "/" +
                    std::to_string(rtp_stats.access_units_dropped) +
                    " frameQueue size/dropTiming/dropOverflow/hold/reuse=" +
                    std::to_string(frame_holder.getFrameQueueSize()) + "/" +
                    std::to_string(frame_holder.getTimingDropStat()) + "/" +
                    std::to_string(frame_holder.getOverflowDropStat()) + "/" +
                    std::to_string(frame_holder.getTimingHoldStat()) + "/" +
                    std::to_string(frame_holder.getFakeFrameStat()));
}


VideoPerformanceCounters WebRtcSession::get_video_performance() const {
    VideoPerformanceCounters result;
    result.access_units = static_cast<uint64_t>(packets_received_.load(std::memory_order_relaxed));
    result.access_unit_bytes = video_access_unit_bytes_.load(std::memory_order_relaxed);
    result.decoded_frames = static_cast<uint64_t>(frames_decoded_.load(std::memory_order_relaxed));
    result.presented_frames = presented_frames_.load(std::memory_order_relaxed);
    result.decode_us_p95 = LatencyPercentile95(decode_latency_buckets_);
    result.decode_us_max = decode_us_max_.load(std::memory_order_relaxed);
    result.queue_wait_us_p95 = LatencyPercentile95(queue_wait_buckets_);
    result.queue_wait_us_max = queue_wait_us_max_.load(std::memory_order_relaxed);
    result.render_us_p95 = LatencyPercentile95(render_latency_buckets_);
    result.render_us_max = render_us_max_.load(std::memory_order_relaxed);
    result.decode_queue_high_water = decoder_queue_high_water_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> queue_lock(decoder_queue_mutex_);
        result.decode_queue_size = decoder_queue_.size();
    }
    return result;
}

int WebRtcSession::get_network_rtt_ms() const {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    return pc_ ? peer_connection_get_rtt_ms(pc_) : -1;
}

StreamNetworkCounters WebRtcSession::get_network_counters() const {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    PeerVideoRtpStats stats {};
    if (pc_)
        (void)peer_connection_get_video_rtp_stats(pc_, &stats);

    StreamNetworkCounters counters;
    counters.packets_received = stats.packets_received;
    counters.sequence_gaps = stats.sequence_gaps;
    counters.late_packets_dropped = stats.late_packets_dropped;
    counters.access_units_dropped = stats.access_units_dropped;
    counters.nack_requests = stats.nack_requests;
    return counters;
}

StreamTransportHealth WebRtcSession::get_transport_health() const {
    StreamTransportHealth health;
    health.peer_completed = peer_ever_completed_.load(std::memory_order_acquire);
    health.peer_terminal = static_cast<opennow::PeerTerminalKind>(
        peer_terminal_kind_.load(std::memory_order_acquire));
    health.signaling_connected = signaling_client_ && signaling_client_->is_connected();
    const uint64_t last_packet_us = last_video_packet_at_us_.load(std::memory_order_acquire);
    health.video_started = last_packet_us != 0;
    if (last_packet_us != 0) {
        const uint64_t now_us = NowUs();
        health.video_idle = std::chrono::milliseconds(
            now_us > last_packet_us ? (now_us - last_packet_us) / 1000 : 0);
    }
    return health;
}

std::string WebRtcSession::get_debug_info() const {
    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    char buf[1800];
    std::string msg_log;
    for (const auto& m : last_messages_) {
        msg_log += m + "\n";
    }
    int pair_total = 0;
    int pair_frozen = 0;
    int pair_checking = 0;
    int pair_succeeded = 0;
    int pair_failed = 0;
    if (pc_) {
        peer_connection_get_ice_candidate_pair_stats(
            pc_, &pair_total, &pair_frozen, &pair_checking, &pair_succeeded, &pair_failed);
    }
    size_t decode_queue_size = 0;
    size_t decode_pool_size = 0;
    {
        std::lock_guard<std::mutex> queue_lock(decoder_queue_mutex_);
        decode_queue_size = decoder_queue_.size();
        decode_pool_size = decoder_buffer_pool_.size();
    }
    PeerVideoRtpStats rtp_stats {};
    if (pc_)
        (void)peer_connection_get_video_rtp_stats(pc_, &rtp_stats);
    const auto& holder = AVFrameHolder::instance();
    const VideoPerformanceCounters video = get_video_performance();
    snprintf(buf, sizeof(buf),
             "URL: %s\nMedia: %s:%d\nICE servers: %zu\nPreset: %s %dx%d@%d %d kbps\nState: %s\nSignaling RX: %d  Offers: %d  ICE remote/local: %d/%d  HB rx/tx: %d/%d\nICE pairs total/frozen/checking/ok/failed: %d/%d/%d/%d/%d\nDataChannel SCTP/requested/attempts/start: %d/%d/%d/%d\nInput rel/pr/ready/proto/hb/threshold: %d/%d/%d/%d/%d/%d\nXbox reports/mouse clicks: %d/%d\nVideo backend: %s\nVideo access units/bytes/max: %d/%llu/%llu\nDecoded/presented/errors: %d/%llu/%d\nDecode us p95/max: %llu/%llu  queue wait p95/max: %llu/%llu\nRender us p95/max: %llu/%llu\nDecode queue/current/high/dropped: %zu/%zu/%d\nFrame queue: %zu  Dropped: %zu  Reused: %zu\n\nLast Messages:\n%s",
             signaling_url_.c_str(),
             media_ip_.empty() ? "(auto)" : media_ip_.c_str(),
             media_port_,
             ice_servers_.size(),
             settings_.label.c_str(),
             settings_.width,
             settings_.height,
             settings_.fps,
             settings_.bitrate_kbps,
             current_state_.c_str(),
             signaling_rx_count_,
             offer_count_,
             remote_ice_count_,
             local_ice_count_,
             heartbeat_rx_count_,
             heartbeat_tx_count_,
             pair_total,
             pair_frozen,
             pair_checking,
             pair_succeeded,
             pair_failed,
             datachannel_opened_ ? 1 : 0,
             datachannel_open_requested_ ? 1 : 0,
             datachannel_open_attempts_,
             startup_control_sent_ ? 1 : 0,
             reliable_input_channel_requested_ ? 1 : 0,
             partial_input_channel_requested_ ? 1 : 0,
             input_ready_ ? 1 : 0,
             input_protocol_version_,
             input_heartbeat_tx_count_,
             partial_reliable_threshold_ms_,
             gamepad_tx_count_,
             mouse_tx_count_,
             video_backend_name_.c_str(),
             packets_received_.load(),
             static_cast<unsigned long long>(video.access_unit_bytes),
             static_cast<unsigned long long>(video_access_unit_max_bytes_.load()),
             frames_decoded_.load(),
             static_cast<unsigned long long>(video.presented_frames),
             decode_errors_.load(),
             static_cast<unsigned long long>(video.decode_us_p95),
             static_cast<unsigned long long>(video.decode_us_max),
             static_cast<unsigned long long>(video.queue_wait_us_p95),
             static_cast<unsigned long long>(video.queue_wait_us_max),
             static_cast<unsigned long long>(video.render_us_p95),
             static_cast<unsigned long long>(video.render_us_max),
             decode_queue_size,
             video.decode_queue_high_water,
             decoder_queue_drops_.load(),
             holder.getFrameQueueSize(),
             holder.getFrameDropStat(),
             holder.getFakeFrameStat(),
             msg_log.c_str());
    std::string result(buf);
    result += "\nAU buffers pool/reuse/alloc: " + std::to_string(decode_pool_size) + "/" +
              std::to_string(decoder_buffer_reuses_.load()) + "/" +
              std::to_string(decoder_buffer_allocations_.load());
    result += "\nRTP packets/gaps/reordered/late/forced: " +
              std::to_string(rtp_stats.packets_received) + "/" +
              std::to_string(rtp_stats.sequence_gaps) + "/" +
              std::to_string(rtp_stats.reordered_packets) + "/" +
              std::to_string(rtp_stats.late_packets_dropped) + "/" +
              std::to_string(rtp_stats.forced_sequence_skips);
    result += "\nRTP access units ok/dropped: " +
              std::to_string(rtp_stats.access_units_completed) + "/" +
              std::to_string(rtp_stats.access_units_dropped);
    result += "\nRTCP NACK requests/packets: " +
              std::to_string(rtp_stats.nack_requests) + "/" +
              std::to_string(rtp_stats.nack_packets_requested);
    result += "\nFrame timing drop/overflow/hold: " +
              std::to_string(holder.getTimingDropStat()) + "/" +
              std::to_string(holder.getOverflowDropStat()) + "/" +
              std::to_string(holder.getTimingHoldStat());
    if (audio_) {
        result += "\n" + audio_->debug_info();
        const int64_t target = video_target_rtp_timestamp();
        const int64_t rendered = last_rendered_video_pts_.load();
        if (target != AV_NOPTS_VALUE && rendered != AV_NOPTS_VALUE) {
            const int32_t delta_ticks = static_cast<int32_t>(
                static_cast<uint32_t>(rendered) - static_cast<uint32_t>(target));
            result += "\nA/V delta: " + std::to_string(delta_ticks / 90) + " ms";
        } else {
            result += "\nA/V delta: waiting for RTCP SR";
        }
    }
    return result;
}
