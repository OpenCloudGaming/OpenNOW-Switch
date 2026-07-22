#include "webrtc_session.hpp"
#include "stream_diagnostics.hpp"
#include "network_loop_policy.hpp"
#include "stream/ffmpeg/AVFrameHolder.hpp"
#include "borealis/core/logger.hpp"
#include "internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <thread>

#ifdef __SWITCH__
#include <sys/stat.h>
#endif

using namespace opennow::webrtc::internal;


namespace
{

constexpr const char* kNvdecActiveMarker =
    "sdmc:/switch/SwitchNOW/nvdec_active.marker";

bool ConsumeNvdecCrashMarker()
{
    std::ifstream marker(kNvdecActiveMarker, std::ios::binary);
    const bool exists = marker.good();
    marker.close();
    if (exists)
        std::remove(kNvdecActiveMarker);
    return exists;
}

bool CreateNvdecCrashMarker()
{
#ifdef __SWITCH__
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchNOW", 0777);
#endif
    std::ofstream marker(kNvdecActiveMarker, std::ios::binary | std::ios::trunc);
    if (!marker.is_open())
        return false;
    marker << "NVDEC stream active; remove after clean shutdown\n";
    marker.flush();
    return marker.good();
}

std::string BuildSignInUrl(std::string base_url, const std::string& peer_name, const std::string& session_id)
{
    const size_t query_pos = base_url.find('?');
    if (query_pos != std::string::npos)
        base_url.erase(query_pos);

    if (StartsWith(base_url, "http://"))
        base_url.replace(0, 4, "ws");
    else if (StartsWith(base_url, "https://"))
        base_url.replace(0, 5, "wss");

    while (!base_url.empty() && base_url.back() == '/')
        base_url.pop_back();

    if (base_url.size() < 8 || base_url.substr(base_url.size() - 8) != "sign_in")
        base_url += "/sign_in";

    return base_url + "?peer_id=" + peer_name + "&version=2&peer_role=1&pairing_id=" + session_id;
}

std::string MakePeerName()
{
    std::random_device rd;
    const auto ticks = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return "peer-" + std::to_string((ticks ^ rd()) % 10000000000ULL);
}

} // namespace

WebRtcSession::WebRtcSession(
    const std::string& signaling_url,
    const std::string& jwt_token,
    const std::string& session_id,
    const std::string& media_ip,
    int media_port,
    const std::vector<opennow::IceServerInfo>& ice_servers)
    : signaling_url_(signaling_url),
      jwt_token_(jwt_token),
      session_id_(session_id),
      media_ip_(media_ip),
      media_port_(media_port),
      ice_servers_(ice_servers),
      settings_(opennow::LoadStreamSettings()),
      peer_name_(MakePeerName()) {
    opennow::SetStreamDiagnosticsEnabled(settings_.debug_diagnostics);

    // Initialize libpeer
    peer_init();
    renderer_ = std::make_unique<DKVideoRenderer>();
    audio_ = std::make_unique<AudioPipeline>();
    audio_->configure(settings_.audio_volume, settings_.audio_buffer_ms);

    const bool previous_nvdec_crash = settings_.video_backend == "Auto" &&
                                      ConsumeNvdecCrashMarker();
    auto_safe_mode_used_ = previous_nvdec_crash;
    const bool force_software = settings_.video_backend == "Software" ||
                                previous_nvdec_crash;
    decoder_ = std::make_unique<FFmpegVideoDecoder>();
    decoder_setup_result_ = decoder_->setup(
        VIDEO_FORMAT_H264, settings_.width, settings_.height, settings_.fps, nullptr,
        force_software ? VIDEO_DECODER_FORCE_SOFTWARE : VIDEO_DECODER_PREFER_HARDWARE);

    if (decoder_setup_result_ != 0 && !force_software) {
        decoder_->cleanup();
        decoder_ = std::make_unique<FFmpegVideoDecoder>();
        decoder_setup_result_ = decoder_->setup(
            VIDEO_FORMAT_H264, settings_.width, settings_.height, settings_.fps, nullptr,
            VIDEO_DECODER_FORCE_SOFTWARE);
        decoder_fallback_used_ = decoder_setup_result_ == 0;
    }

    if (decoder_setup_result_ == 0 && decoder_->uses_hardware_frames())
        video_backend_name_ = "NVDEC-NVTEGRA/Deko3D-zero-copy";
    else if (decoder_setup_result_ == 0)
        video_backend_name_ = auto_safe_mode_used_
            ? "FFmpeg-SW-3T/Deko3D-upload(crash-safe-mode)"
            : decoder_fallback_used_
            ? "FFmpeg-SW-3T/Deko3D-upload(auto-fallback)"
            : "FFmpeg-SW-3T/Deko3D-upload";
    else
        video_backend_name_ = "decoder-setup-failed";

    if (decoder_setup_result_ == 0 && decoder_->uses_hardware_frames())
        nvdec_marker_owned_ = CreateNvdecCrashMarker();
}

WebRtcSession::~WebRtcSession() {
    stop();
    peer_deinit();
}

void WebRtcSession::setup_peer_connection() {
    PeerConfiguration config = {};
    config.video_codec = CODEC_H264;
    config.audio_codec = CODEC_OPUS;
    config.datachannel = DATA_CHANNEL_STRING;
    config.onvideopacket = on_video_packet_cb;
    config.onaudiopacket = on_audio_packet_cb;
    config.onrtpsenderreport = on_rtp_sender_report_cb;
    config.user_data = this;

    if (ice_servers_.empty()) {
        ice_servers_.push_back({"stun:s1.stun.gamestream.nvidia.com:19308", "", ""});
        ice_servers_.push_back({"stun:stun.l.google.com:19302", "", ""});
        ice_servers_.push_back({"stun:stun1.l.google.com:19302", "", ""});
    }

    const size_t ice_count = std::min<size_t>(ice_servers_.size(), 5);
    for (size_t i = 0; i < ice_count; ++i) {
        config.ice_servers[i].urls = ice_servers_[i].url.c_str();
        config.ice_servers[i].username =
            ice_servers_[i].username.empty() ? nullptr : ice_servers_[i].username.c_str();
        config.ice_servers[i].credential =
            ice_servers_[i].credential.empty() ? nullptr : ice_servers_[i].credential.c_str();
    }

    pc_ = peer_connection_create(&config);
    if (!pc_) {
        brls::Logger::error("Failed to create PeerConnection");
        return;
    }

    peer_connection_onicecandidate(pc_, on_ice_candidate_cb);
    peer_connection_oniceconnectionstatechange(pc_, on_peer_state_change_cb);
    peer_connection_ondatachannel(pc_, on_datachannel_message_cb, on_datachannel_open_cb, on_datachannel_close_cb);
    // Note: libpeer requires patching or explicit callback for track data
    // Assuming on_track or similar is available. For now, we stub it.
    // peer_connection_ontrack(pc_, on_track_cb, this);
}

void WebRtcSession::start() {
    stop_requested_.store(false, std::memory_order_release);
    session_started_at_ = std::chrono::steady_clock::now();
    ResetStreamTraceLog();
    AppendStreamLog("SESSION start url=" + signaling_url_ +
                    " media=" + (media_ip_.empty() ? std::string("(auto)") : media_ip_) +
                    ":" + std::to_string(media_port_) +
                    " preset=" + settings_.label +
                    " " + std::to_string(settings_.width) + "x" +
                    std::to_string(settings_.height) + "@" +
                    std::to_string(settings_.fps) +
                    " bitrate=" + std::to_string(settings_.bitrate_kbps) +
                    " iceServers=" + std::to_string(ice_servers_.size()));
    AppendStreamLog("VIDEO backend=" + video_backend_name_ +
                    " requested=" + settings_.video_backend +
                    " fallback=" + std::to_string(decoder_fallback_used_ ? 1 : 0) +
                    " crashSafeMode=" + std::to_string(auto_safe_mode_used_ ? 1 : 0) +
                    " decoderSetup=" + std::to_string(decoder_setup_result_) +
                    " expectedFormat=" +
                    std::string(decoder_ && decoder_->uses_hardware_frames()
                        ? "NVTEGRA" : "YUV420P/NV12"));
    const bool input_codec_ok = InputEncodingSelfTest();
    AppendStreamLog("INPUT codec_selftest ok=" + std::to_string(input_codec_ok ? 1 : 0));
    AppendInputLog("SESSION start codecSelfTest=" + std::to_string(input_codec_ok ? 1 : 0) +
                   " expectedController=xbox fixedReliableSid=0");
    AppendTraceLog("SESSION start");
    AppendTraceLog("url=" + signaling_url_);
    AppendTraceLog("sessionId=" + session_id_);
    AppendTraceLog("peerName=" + peer_name_);
    AppendTraceLog("mediaHint=" + (media_ip_.empty() ? std::string("(auto)") : media_ip_) +
                   ":" + std::to_string(media_port_));
    AppendTraceLog("preset=" + settings_.label + " resolution=" +
                   std::to_string(settings_.width) + "x" + std::to_string(settings_.height) +
                   " fps=" + std::to_string(settings_.fps) +
                   " bitrateKbps=" + std::to_string(settings_.bitrate_kbps));
    for (size_t i = 0; i < ice_servers_.size(); ++i) {
        AppendTraceLog("iceServer[" + std::to_string(i) + "] url=" + ice_servers_[i].url +
                       " user=" + (ice_servers_[i].username.empty() ? std::string("(empty)") : std::string("(set)")) +
                       " credential=" + (ice_servers_[i].credential.empty() ? std::string("(empty)") : std::string("(set)")));
    }
    setup_peer_connection();
    if (!pc_) {
        current_state_ = "PeerConnection setup failed";
        AppendStreamLog("SESSION error peer_connection_setup_failed");
        return;
    }

    const std::string sign_in_url = BuildSignInUrl(signaling_url_, peer_name_, session_id_);
    signaling_client_ = std::make_unique<SignalingClient>(sign_in_url);
    signaling_client_->set_on_message([this](const std::string& msg) {
        handle_signaling_message(msg);
    });

    std::vector<std::string> headers;
    headers.push_back("Origin: https://play.geforcenow.com");
    headers.push_back("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/131.0.0.0 Safari/537.36");
    if (!session_id_.empty()) {
        headers.push_back("Sec-WebSocket-Protocol: x-nv-sessionid." + session_id_);
    }
    signaling_client_->set_custom_headers(headers);

    if (!signaling_client_->connect()) {
        current_state_ = "WebSocket Connect Failed: " + signaling_client_->get_last_error();
        AppendStreamLog("SESSION error websocket_connect_failed " + signaling_client_->get_last_error());
        signaling_ready_ = false;
        return;
    }
    signaling_ready_ = true;
    current_state_ = "Signaling connected, waiting for offer";
    AppendStreamLog("SIGNAL connected");
    send_peer_info();
    send_heartbeat();
    if (settings_.audio_enabled)
        audio_->start();
    start_decoder_worker();
    start_network_worker();
}


void WebRtcSession::start_network_worker() {
    if (network_running_.exchange(true))
        return;

    network_thread_ = std::thread(&WebRtcSession::network_loop, this);
    AppendStreamLog("TRANSPORT worker_started");
}

void WebRtcSession::network_loop() {
    while (network_running_.load(std::memory_order_acquire)) {
        bool can_run = false;
        int batch_size = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
            can_run = pc_ && signaling_ready_ && remote_description_set_ &&
                      (remote_ice_count_ > 0 || manual_candidate_added_);
            if (can_run) {
                // Motion-heavy frames arrive as short UDP bursts. Drain a
                // bounded batch while the peer lock is already held instead
                // of paying one mutex hand-off and scheduler yield per packet.
                constexpr int kMaximumDatagramsPerBatch = 24;
                constexpr auto kMaximumBatchTime = std::chrono::microseconds(1000);
                const auto batch_started_at = std::chrono::steady_clock::now();
                for (int packet = 0; packet < kMaximumDatagramsPerBatch; ++packet) {
                    if (peer_connection_loop(pc_) == 0)
                        break;
                    batch_size++;
                    if (batch_size >= 8 &&
                        std::chrono::steady_clock::now() - batch_started_at >=
                            kMaximumBatchTime) {
                        break;
                    }
                }
                if (batch_size > 0) {
                    transport_batches_.fetch_add(1, std::memory_order_relaxed);
                    transport_datagrams_.fetch_add(
                        static_cast<uint64_t>(batch_size), std::memory_order_relaxed);
                    AtomicMax(transport_batch_high_water_, static_cast<size_t>(batch_size));
                }
            }
        }

        const int backoff_ms =
            opennow::network::LoopBackoffMilliseconds(can_run, batch_size);
        if (backoff_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        else
            std::this_thread::yield();
    }

    AppendStreamLog("TRANSPORT worker_stopped");
}


void WebRtcSession::poll() {
    if (stop_requested_.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);

    if (signaling_client_) {
        signaling_client_->poll();
    }

    maybe_send_keepalive();

    if (pc_ && signaling_ready_) {
        if (!remote_description_set_) {
            current_state_ = "Signaling connected, waiting for offer";
            return;
        }

        maybe_add_manual_media_candidate();

        if (remote_ice_count_ == 0 && !manual_candidate_added_) {
            current_state_ = "Waiting for remote ICE";
            return;
        }

        const PeerConnectionState state = peer_connection_get_state(pc_);
        current_state_ = std::string("Peer ") + peer_connection_state_to_string(state);
        if (state == PEER_CONNECTION_COMPLETED) {
            if (keyframe_needed_.exchange(false) && decoder_resync_required_.load())
                request_keyframe("decoder_resync");
            maybe_open_datachannel();
            maybe_activate_input();
            maybe_send_startup_control_messages();
            maybe_send_input_heartbeat();
            maybe_recover_rtp_damage();
            maybe_recover_decode_stall();
        }
        if (state == PEER_CONNECTION_COMPLETED && !initial_keyframe_requested_) {
            last_startup_keyframe_request_ = std::chrono::steady_clock::now();
            request_keyframe("stream_start");
            initial_keyframe_requested_ = true;
        }
        maybe_request_startup_keyframe_retry();
        maybe_log_stream_diagnostics();
    }
}


void WebRtcSession::request_stop() {
    if (stop_requested_.exchange(true, std::memory_order_acq_rel))
        return;

    network_running_.store(false, std::memory_order_release);
    decoder_running_.store(false, std::memory_order_release);
    decoder_queue_cv_.notify_all();
    AppendInputLog("SESSION asynchronous_stop_requested");
}

void WebRtcSession::stop() {
    request_stop();
    if (network_thread_.joinable())
        network_thread_.join();

    if (audio_)
        audio_->stop();

    if (decoder_thread_.joinable())
        decoder_thread_.join();

    {
        std::lock_guard<std::mutex> queue_lock(decoder_queue_mutex_);
        clear_decoder_queue_locked();
        decoder_buffer_pool_.clear();
    }

    std::lock_guard<std::recursive_mutex> lock(peer_mutex_);
    if (!pc_ && !decoder_)
        return;

    log_stream_summary("stop");
    AppendInputLog("SUMMARY attempts=" + std::to_string(gamepad_input_attempt_count_) +
                   " blocked=" + std::to_string(gamepad_input_blocked_count_) +
                   " reportsSent=" + std::to_string(gamepad_tx_count_) +
                   " sendFailures=" + std::to_string(gamepad_send_failure_count_) +
                   " mouseSent=" + std::to_string(mouse_tx_count_) +
                   " sctpOpen=" + std::to_string(datachannel_opened_ ? 1 : 0) +
                   " channelRequested=" + std::to_string(datachannel_open_requested_ ? 1 : 0) +
                   " inputReady=" + std::to_string(input_ready_ ? 1 : 0) +
                   " protocol=" + std::to_string(input_protocol_version_));
    if (pc_) {
        peer_connection_close(pc_);
        peer_connection_destroy(pc_);
        pc_ = nullptr;
    }
    // Zero-copy Deko3D mappings reference NVDEC-owned memory, so release all
    // renderer mappings before destroying the decoder hardware frame pool.
    if (renderer_)
        renderer_.reset();
    if (decoder_) {
        decoder_->cleanup();
        decoder_.reset();
    }
    if (nvdec_marker_owned_) {
        std::remove(kNvdecActiveMarker);
        nvdec_marker_owned_ = false;
    }
}
