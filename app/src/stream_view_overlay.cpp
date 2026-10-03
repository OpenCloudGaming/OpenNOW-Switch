#include "StreamView.hpp"
#include "ui_theme.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

void StreamView::DrawDebugOverlay(NVGcontext* vg, float x, float y, float width) {
    if (!session_)
        return;

    std::string debug_text = session_->get_debug_info();
    nvgFontSize(vg, 24.0f);
    nvgFontFaceId(vg, opennow::ui::Font(opennow::ui::FontRole::Body));
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgTextBox(vg, x + 50.0f, y + 50.0f, width - 100.0f, debug_text.c_str(), nullptr);
}

void StreamView::DrawPreparingStream(
    NVGcontext* vg, float x, float y, float width, float height) {
    constexpr float kPi = 3.14159265358979323846f;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    const StreamTransportHealth health = session_
        ? session_->get_transport_health()
        : StreamTransportHealth {};
    const VideoPerformanceCounters counters = session_
        ? session_->get_video_performance()
        : VideoPerformanceCounters {};

    int stage = 1;
    std::string title = "Connecting to your cloud rig...";
    std::string detail = "Opening the secure signaling channel";
    if (health.signaling_connected)
    {
        stage = 1;
        title = "Negotiating the stream...";
        detail = "Selecting the fastest network path";
    }
    if (health.peer_completed)
    {
        stage = 2;
        title = "Connection ready...";
        detail = "Waiting for the first video packets";
    }
    if (counters.access_units > 0)
    {
        stage = 2;
        title = counters.decoded_frames > 0
            ? "Preparing the first clean frame..."
            : "Decoding the video stream...";
        detail = counters.decoded_frames > 0
            ? "The game image will appear automatically"
            : "Video is arriving and the decoder is synchronizing";
    }

    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillColor(vg, nvgRGB(10, 13, 17));
    nvgFill(vg);

    const float glow_x = x + width * 0.5f + static_cast<float>(std::sin(seconds * 0.45)) * 85.0f;
    const float glow_y = y + height * 0.58f + static_cast<float>(std::cos(seconds * 0.38)) * 42.0f;
    nvgBeginPath(vg);
    nvgCircle(vg, glow_x, glow_y, 210.0f);
    nvgFillColor(vg, nvgRGBA(42, 204, 116, 12));
    nvgFill(vg);

    const float panel_width = std::min(680.0f, width - 72.0f);
    const float panel_height = 410.0f;
    const float panel_x = x + (width - panel_width) * 0.5f;
    const float panel_y = y + (height - panel_height) * 0.5f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, panel_x, panel_y, panel_width, panel_height, 20.0f);
    nvgFillColor(vg, nvgRGBA(16, 18, 22, 248));
    nvgFill(vg);

    nvgFontFaceId(vg, opennow::ui::Font(opennow::ui::FontRole::Body));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, opennow::ui::Green());
    nvgText(vg, panel_x + 42.0f, panel_y + 45.0f, "NOW LOADING", nullptr);
    nvgFontSize(vg, 28.0f);
    nvgFillColor(vg, nvgRGB(241, 245, 247));
    const std::string game = game_title_.empty() ? "GeForce NOW session" : game_title_;
    nvgText(vg, panel_x + 42.0f, panel_y + 82.0f, game.c_str(), nullptr);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, nvgRGB(142, 150, 160));
    nvgText(vg, panel_x + 42.0f, panel_y + 112.0f, "GeForce NOW cloud stream", nullptr);

    static constexpr const char* kSteps[] = {"QUEUE", "SETUP", "READY"};
    const float rail_y = panel_y + 178.0f;
    const float first_x = panel_x + 88.0f;
    const float gap = (panel_width - 176.0f) * 0.5f;
    nvgBeginPath(vg);
    nvgRect(vg, first_x, rail_y - 2.0f, gap * 2.0f, 4.0f);
    nvgFillColor(vg, nvgRGB(37, 41, 47));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, first_x, rail_y - 2.0f, gap * std::min(stage, 2), 4.0f);
    nvgFillColor(vg, opennow::ui::Green());
    nvgFill(vg);

    const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(seconds * 3.1));
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    for (int i = 0; i < 3; ++i)
    {
        const bool complete = i < stage;
        const bool active = i == stage;
        const float cx = first_x + i * gap;
        if (active)
        {
            nvgBeginPath(vg);
            nvgCircle(vg, cx, rail_y, 29.0f + pulse * 3.0f);
            nvgFillColor(vg, nvgRGBA(118, 232, 58, 22 + static_cast<int>(pulse * 24.0f)));
            nvgFill(vg);
        }
        nvgBeginPath(vg);
        nvgCircle(vg, cx, rail_y, 22.0f);
        nvgFillColor(vg, complete || active ? opennow::ui::Green() : nvgRGB(24, 28, 33));
        nvgFill(vg);
        nvgStrokeWidth(vg, 2.0f);
        nvgStrokeColor(vg, complete || active ? nvgRGB(108, 235, 153) : nvgRGB(43, 48, 55));
        nvgStroke(vg);
        nvgFontSize(vg, 16.0f);
        nvgFillColor(vg, complete || active ? nvgRGB(8, 35, 22) : nvgRGB(95, 101, 111));
        const std::string number = std::to_string(i + 1);
        nvgText(vg, cx, rail_y, number.c_str(), nullptr);
        nvgFontSize(vg, 12.0f);
        nvgFillColor(vg, complete || active ? nvgRGB(225, 232, 229) : nvgRGB(91, 96, 105));
        nvgText(vg, cx, rail_y + 42.0f, kSteps[i], nullptr);
    }

    const float spinner_x = panel_x + panel_width * 0.5f;
    const float spinner_y = panel_y + 278.0f;
    const float angle = static_cast<float>(
        std::fmod(seconds * 3.0, static_cast<double>(kPi) * 2.0));
    nvgBeginPath(vg);
    nvgCircle(vg, spinner_x, spinner_y, 19.0f);
    nvgStrokeWidth(vg, 5.0f);
    nvgStrokeColor(vg, nvgRGB(35, 43, 48));
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgArc(vg, spinner_x, spinner_y, 19.0f, angle, angle + kPi * 1.42f, NVG_CW);
    nvgStrokeWidth(vg, 5.0f);
    nvgLineCap(vg, NVG_ROUND);
    nvgStrokeColor(vg, opennow::ui::Green());
    nvgStroke(vg);
    const float head_angle = angle + kPi * 1.42f;
    nvgBeginPath(vg);
    nvgCircle(vg, spinner_x + std::cos(head_angle) * 19.0f,
              spinner_y + std::sin(head_angle) * 19.0f, 3.4f);
    nvgFillColor(vg, opennow::ui::Green());
    nvgFill(vg);

    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 20.0f);
    nvgFillColor(vg, nvgRGB(238, 242, 245));
    nvgText(vg, spinner_x, panel_y + 330.0f, title.c_str(), nullptr);
    nvgFontSize(vg, 15.0f);
    nvgFillColor(vg, nvgRGB(145, 153, 164));
    nvgText(vg, spinner_x, panel_y + 361.0f, detail.c_str(), nullptr);
}

void StreamView::UpdatePerformanceCounter() {
    const auto now = std::chrono::steady_clock::now();
    if (fps_window_started_.time_since_epoch().count() == 0) {
        fps_window_started_ = now;
        previous_video_counters_ = session_
            ? session_->get_video_performance() : VideoPerformanceCounters {};
        return;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - fps_window_started_);
    if (elapsed >= std::chrono::milliseconds(750)) {
        const VideoPerformanceCounters counters = session_
            ? session_->get_video_performance() : VideoPerformanceCounters {};
        const float scale = elapsed.count() > 0 ? 1000.0f / static_cast<float>(elapsed.count()) : 0.0f;
        const auto delta = [](uint64_t current, uint64_t previous) {
            return current >= previous ? current - previous : uint64_t {0};
        };
        incoming_fps_ = static_cast<float>(
            delta(counters.access_units, previous_video_counters_.access_units)) * scale;
        decoded_fps_ = static_cast<float>(
            delta(counters.decoded_frames, previous_video_counters_.decoded_frames)) * scale;
        presented_fps_ = static_cast<float>(
            delta(counters.presented_frames, previous_video_counters_.presented_frames)) * scale;
        stream_bitrate_mbps_ = static_cast<float>(
            delta(counters.access_unit_bytes, previous_video_counters_.access_unit_bytes)) *
            scale * 8.0f / 1000000.0f;
        network_rtt_ms_ = session_ ? session_->get_network_rtt_ms() : -1;
        network_counters_ =
            session_ ? session_->get_network_counters() : StreamNetworkCounters {};
        previous_video_counters_ = counters;
        fps_window_started_ = now;
    }
}

void StreamView::DrawPerformanceOverlay(NVGcontext* vg, float x, float y) {
    char text[128];
    if (network_rtt_ms_ >= 0) {
        std::snprintf(
            text, sizeof(text), "FPS  %.0f     BITRATE  %.1f Mbps     PING  %d ms",
            presented_fps_, stream_bitrate_mbps_, network_rtt_ms_);
    } else {
        std::snprintf(
            text, sizeof(text), "FPS  %.0f     BITRATE  %.1f Mbps     PING  -- ms",
            presented_fps_, stream_bitrate_mbps_);
    }

    const float box_x = x + 16.0f;
    const float box_y = y + 16.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, 520.0f, 46.0f, 8.0f);
    nvgFillColor(vg, nvgRGBA(8, 12, 14, 205));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x + 6.0f, box_y + 7.0f, 4.0f, 32.0f, 2.0f);
    nvgFillColor(vg, opennow::ui::Green());
    nvgFill(vg);

    nvgFontSize(vg, 18.0f);
    nvgFontFaceId(vg, opennow::ui::Font(opennow::ui::FontRole::Body));
    nvgFillColor(vg, nvgRGB(245, 250, 247));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, box_x + 20.0f, box_y + 23.0f, text, nullptr);
}

void StreamView::SetStreamOverlayVisible(bool visible)
{
    if (stream_overlay_visible_ == visible)
        return;

    stream_overlay_visible_ = visible;
    stream_overlay_b_was_down_ = false;
    if (visible)
        UpdateGameplayInputCapture(true, std::chrono::steady_clock::now());
    if (session_)
    {
        session_->record_ui_event(
            visible ? "stream overlay opened by Minus+Plus"
                    : "stream overlay closed");
    }

    if (!visible)
        keyboard_release_guard_ = true;
}

void StreamView::DrawControllerNotice(
    NVGcontext* vg, float x, float y, float width,
    std::chrono::steady_clock::time_point now)
{
    UpdateControllerNotice(now);
    if (controller_notice_text_.empty())
        return;

    const float box_width = std::min(360.0f, width - 36.0f);
    const float box_height = 54.0f;
    const float box_x = x + (width - box_width) * 0.5f;
    const float box_y = y + 18.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, box_width, box_height, 13.0f);
    nvgFillColor(vg, nvgRGBA(7, 11, 14, 232));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, 6.0f, box_height, 3.0f);
    nvgFillColor(vg, opennow::ui::Green());
    nvgFill(vg);

    nvgFontSize(vg, 21.0f);
    nvgFontFaceId(vg, opennow::ui::Font(opennow::ui::FontRole::Body));
    nvgFillColor(vg, nvgRGB(248, 252, 249));
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgText(
        vg, box_x + box_width * 0.5f, box_y + box_height * 0.5f,
        controller_notice_text_.c_str(), nullptr);
}

void StreamView::RefreshNetworkInfo(std::chrono::steady_clock::time_point now)
{
    const bool was_2_4_ghz =
        opennow::network::ShouldWarnForStreaming(network_info_.wifi_band);
    const opennow::NetworkMonitorSnapshot snapshot = network_monitor_.snapshot();
    network_info_ = snapshot.connection_info;
    internet_connected_ = snapshot.internet_connected;

    const bool is_2_4_ghz =
        opennow::network::ShouldWarnForStreaming(network_info_.wifi_band);
    if (is_2_4_ghz && !was_2_4_ghz) {
        network_warning_visible_until_ = now + std::chrono::seconds(12);
        if (session_)
            session_->record_ui_event("network_warning wifiBand=2.4GHz");
    }
}

void StreamView::DrawNetworkWarning(
    NVGcontext* vg, float x, float y, float width, float height,
    std::chrono::steady_clock::time_point now)
{
    if (stream_overlay_visible_ || now >= network_warning_visible_until_ ||
        !opennow::network::ShouldWarnForStreaming(network_info_.wifi_band))
        return;

    const float box_width = std::min(650.0f, width - 36.0f);
    const float box_height = 72.0f;
    const float box_x = x + (width - box_width) * 0.5f;
    const float box_y = y + height - box_height - 76.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, box_width, box_height, 13.0f);
    nvgFillColor(vg, nvgRGBA(13, 16, 18, 238));
    nvgFill(vg);
    nvgStrokeWidth(vg, 1.0f);
    nvgStrokeColor(vg, nvgRGBA(255, 184, 58, 105));
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, 6.0f, box_height, 3.0f);
    nvgFillColor(vg, nvgRGB(255, 184, 58));
    nvgFill(vg);

    nvgFontFaceId(vg, opennow::ui::Font(opennow::ui::FontRole::Body));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 18.0f);
    nvgFillColor(vg, nvgRGB(252, 244, 224));
    nvgText(vg, box_x + 22.0f, box_y + 25.0f,
            "2.4 GHz Wi-Fi may cause poor streaming performance", nullptr);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, nvgRGB(190, 194, 196));
    nvgText(vg, box_x + 22.0f, box_y + 49.0f,
            "For lower latency and fewer frame drops, use 5 GHz Wi-Fi or Ethernet.", nullptr);
}

void StreamView::DrawStreamOverlay(
    NVGcontext* vg, float x, float y, float width, float height,
    std::chrono::steady_clock::time_point now)
{
    if (!stream_overlay_visible_)
        return;
    if (!stream_overlay_view_)
        stream_overlay_view_ = std::make_unique<opennow::StreamOverlayView>();
    if (now >= stream_overlay_next_sample_)
    {
        const auto health = session_ ? session_->get_transport_health() : StreamTransportHealth {};
        const auto& counters = previous_video_counters_;
        const auto& network = network_counters_;
        opennow::StreamOverlaySample sample;
        sample.game_title = game_title_;
        sample.provider = auth_.provider.display_name;
        sample.codec = stream_codec_;
        sample.location = opennow::ui::ConfiguredLocation(stream_region_);
        if (!network_info_.connected)
            sample.network = "Disconnected";
        else if (network_info_.type == opennow::NetworkConnectionType::Ethernet)
            sample.network = "Ethernet";
        else if (network_info_.type == opennow::NetworkConnectionType::Wifi)
            sample.network = opennow::network::WifiBandLabel(network_info_.wifi_band);
        sample.incoming_fps = incoming_fps_;
        sample.decoded_fps = decoded_fps_;
        sample.presented_fps = presented_fps_;
        sample.bitrate_mbps = stream_bitrate_mbps_;
        sample.rtt_ms = network_rtt_ms_;
        sample.width = session_ ? session_->stream_width() : 0;
        sample.height = session_ ? session_->stream_height() : 0;
        sample.decode_us_p95 = counters.decode_us_p95;
        sample.render_us_p95 = counters.render_us_p95;
        sample.queue_size = counters.decode_queue_size;
        sample.queue_high_water = counters.decode_queue_high_water;
        sample.packets_received = network.packets_received;
        sample.sequence_gaps = network.sequence_gaps;
        sample.late_packets_dropped = network.late_packets_dropped;
        sample.dropped_frames = network.access_units_dropped;
        sample.nack_requests = network.nack_requests;
        sample.peer_connected = health.peer_completed;
        sample.signaling_connected = health.signaling_connected;
        sample.wifi_warning = opennow::network::ShouldWarnForStreaming(network_info_.wifi_band);
        sample.nte_session = is_nte_session_;
        stream_overlay_view_->Update(sample);
        stream_overlay_next_sample_ = now + std::chrono::milliseconds(750);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - stream_started_at_).count();
    if (elapsed != stream_overlay_elapsed_seconds_)
    {
        stream_overlay_elapsed_seconds_ = elapsed;
        stream_overlay_view_->UpdateElapsed(static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)));
    }
    stream_overlay_view_->draw(vg, x, y, width, height,
                               brls::Application::getStyle(), nullptr);
}

void StreamView::DrawNteAutoLoginStatus(
    NVGcontext* vg, float x, float y, float width, float height,
    std::chrono::steady_clock::time_point now) {
    if (!is_nte_session_ || now >= nte_status_until_)
        return;

    std::string text = nte_status_;
    if (text.empty()) {
        text = nte_credentials_.valid()
            ? "NTE Auto-login ready: press L + X"
            : "Configure NTE Auto-login on the game page";
    }
    if (nte_stage_ != NteAutoLoginStage::Idle)
        text += "  |  B Cancel";

    const float box_width = std::min(560.0f, width - 36.0f);
    const float box_height = 46.0f;
    const float box_x = x + 18.0f;
    const float box_y = y + height - box_height - 18.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, box_width, box_height, 11.0f);
    nvgFillColor(vg, nvgRGBA(7, 12, 14, 220));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, 6.0f, box_height, 3.0f);
    nvgFillColor(vg, opennow::ui::Green());
    nvgFill(vg);
    nvgFontFaceId(vg, opennow::ui::Font(opennow::ui::FontRole::Body));
    nvgFontSize(vg, 19.0f);
    nvgFillColor(vg, nvgRGB(246, 250, 248));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, box_x + 20.0f, box_y + box_height * 0.5f, text.c_str(), nullptr);
}

void StreamView::UpdateSessionLimitNotice(std::chrono::steady_clock::time_point now) {
    if (!free_tier_session_ || stream_started_at_.time_since_epoch().count() == 0)
        return;

    constexpr auto kFreeSessionLimit = std::chrono::hours(1);
    const auto elapsed = now - stream_started_at_;
    const auto remaining = elapsed < kFreeSessionLimit
        ? std::chrono::duration_cast<std::chrono::seconds>(kFreeSessionLimit - elapsed)
        : std::chrono::seconds(0);

    if (elapsed >= kFreeSessionLimit ||
        (session_ && session_->is_terminal() && elapsed >= std::chrono::minutes(59))) {
        BeginStreamEnd(opennow::StreamEndReason::FreeSessionEnded, now);
        return;
    }

    if (!one_minute_warning_shown_ && remaining <= std::chrono::seconds(60)) {
        one_minute_warning_shown_ = true;
        five_minute_warning_shown_ = true;
        session_limit_notice_ = SessionLimitNotice::OneMinute;
        session_notice_visible_until_ = now + std::chrono::seconds(10);
        if (session_)
            session_->record_ui_event("free_session_warning remainingSeconds=60");
    } else if (!five_minute_warning_shown_ && remaining <= std::chrono::minutes(5)) {
        five_minute_warning_shown_ = true;
        session_limit_notice_ = SessionLimitNotice::FiveMinutes;
        session_notice_visible_until_ = now + std::chrono::seconds(8);
        if (session_)
            session_->record_ui_event("free_session_warning remainingSeconds=300");
    }
}

void StreamView::BeginStreamEnd(
    opennow::StreamEndReason reason,
    std::chrono::steady_clock::time_point now) {
    if (reason == opennow::StreamEndReason::None ||
        stream_end_reason_ != opennow::StreamEndReason::None)
        return;

    stream_end_reason_ = reason;
    network_monitor_.request_stop();
    stream_end_started_at_ = now;
    stream_auto_exit_at_ = now + std::chrono::seconds(15);
    if (reason == opennow::StreamEndReason::FreeSessionEnded) {
        session_limit_ended_ = true;
        session_limit_notice_ = SessionLimitNotice::Ended;
    }

    std::string event = "stream_end reason=";
    switch (reason) {
        case opennow::StreamEndReason::FreeSessionEnded:
            event += "free_session_limit";
            break;
        case opennow::StreamEndReason::NetworkLost:
            event += "network_lost";
            break;
        case opennow::StreamEndReason::ServerDisconnected:
            event += "server_disconnected";
            break;
        case opennow::StreamEndReason::ConnectionFailed:
            event += "connection_failed";
            break;
        case opennow::StreamEndReason::StreamEnded:
            event += "server_ended_stream";
            break;
        case opennow::StreamEndReason::VideoTimedOut:
            event += "video_timeout";
            break;
        case opennow::StreamEndReason::None:
            return;
    }
    event += " autoExitSeconds=15";
    if (session_) {
        session_->record_ui_event(event);
        // Let transport and decoder workers finish during the 15-second
        // notice. Destruction then cannot leave Borealis input blocked.
        session_->request_stop();
    }
}

void StreamView::UpdateStreamEndState(std::chrono::steady_clock::time_point now) {
    if (!session_ || stream_end_reason_ != opennow::StreamEndReason::None)
        return;

    RefreshNetworkInfo(now);

    const StreamTransportHealth health = session_->get_transport_health();
    opennow::StreamEndSignals signals;
    signals.free_tier = free_tier_session_;
    signals.session_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - stream_started_at_);
    signals.internet_connected = internet_connected_;
    signals.peer_completed = health.peer_completed;
    signals.peer_terminal = health.peer_terminal;
    signals.signaling_connected = health.signaling_connected;
    signals.video_started = health.video_started;
    signals.video_idle = health.video_idle;
    BeginStreamEnd(opennow::DetectStreamEnd(signals), now);
}

void StreamView::DrawStreamEndNotice(
    NVGcontext* vg, float x, float y, float width,
    std::chrono::steady_clock::time_point now) {
    if (stream_end_reason_ == opennow::StreamEndReason::None)
        return;

    std::string title;
    std::string detail;
    switch (stream_end_reason_) {
        case opennow::StreamEndReason::FreeSessionEnded:
            title = "Free session ended";
            detail = "The one-hour GeForce NOW limit was reached";
            break;
        case opennow::StreamEndReason::NetworkLost:
            title = "Network connection lost";
            detail = "Check the Switch internet connection";
            break;
        case opennow::StreamEndReason::ServerDisconnected:
            title = "Streaming server disconnected";
            detail = "The connection to the GeForce NOW rig was lost";
            break;
        case opennow::StreamEndReason::ConnectionFailed:
            title = "Stream connection failed";
            detail = "The GeForce NOW rig could not be reached";
            break;
        case opennow::StreamEndReason::StreamEnded:
            title = "Stream ended by server";
            detail = "GeForce NOW closed this streaming session";
            break;
        case opennow::StreamEndReason::VideoTimedOut:
            title = "Video stream timed out";
            detail = "No video packets were received for 15 seconds";
            break;
        case opennow::StreamEndReason::None:
            return;
    }

    const auto remaining_ms = std::max<std::int64_t>(0,
        std::chrono::duration_cast<std::chrono::milliseconds>(stream_auto_exit_at_ - now).count());
    const int remaining_seconds = static_cast<int>((remaining_ms + 999) / 1000);
    detail += "  |  Returning in " + std::to_string(remaining_seconds) + "s";

    const float box_width = std::min(560.0f, width - 36.0f);
    const float box_height = 82.0f;
    const float box_x = x + width - box_width - 18.0f;
    const float box_y = y + 16.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, box_width, box_height, 13.0f);
    nvgFillColor(vg, nvgRGBA(8, 11, 14, 232));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, 7.0f, box_height, 3.5f);
    nvgFillColor(vg, nvgRGB(255, 91, 91));
    nvgFill(vg);
    nvgFontFaceId(vg, opennow::ui::Font(opennow::ui::FontRole::Body));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 23.0f);
    nvgFillColor(vg, nvgRGB(252, 246, 246));
    nvgText(vg, box_x + 22.0f, box_y + 29.0f, title.c_str(), nullptr);
    nvgFontSize(vg, 16.0f);
    nvgFillColor(vg, nvgRGB(188, 195, 204));
    nvgText(vg, box_x + 22.0f, box_y + 59.0f, detail.c_str(), nullptr);
}

void StreamView::DrawSessionLimitNotice(
    NVGcontext* vg, float x, float y, float width,
    std::chrono::steady_clock::time_point now) {
    if (!free_tier_session_ || session_limit_notice_ == SessionLimitNotice::None)
        return;

    constexpr auto kFreeSessionLimit = std::chrono::hours(1);
    const auto elapsed = now - stream_started_at_;
    const int remaining_seconds = static_cast<int>(std::max<int64_t>(0,
        std::chrono::duration_cast<std::chrono::seconds>(kFreeSessionLimit - elapsed).count()));
    const bool prominent = session_limit_notice_ == SessionLimitNotice::Ended ||
                           now < session_notice_visible_until_;
    if (session_limit_notice_ == SessionLimitNotice::FiveMinutes && !prominent)
        return;

    std::string text;
    NVGcolor accent = nvgRGB(255, 184, 58);
    if (session_limit_notice_ == SessionLimitNotice::Ended) {
        text = "Free session ended";
        accent = nvgRGB(255, 91, 91);
    } else if (session_limit_notice_ == SessionLimitNotice::FiveMinutes) {
        text = "Free session: 5 minutes remaining";
    } else {
        text = prominent
            ? "Free session ends in 60 seconds"
            : "Session 0:" + (remaining_seconds < 10 ? std::string("0") : std::string()) +
                std::to_string(remaining_seconds);
        accent = nvgRGB(255, 112, 82);
    }

    const float box_width = prominent ? std::min(390.0f, width - 36.0f) : 190.0f;
    const float box_height = prominent ? 50.0f : 40.0f;
    const float box_x = x + width - box_width - 18.0f;
    const float box_y = y + 16.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, box_width, box_height, prominent ? 12.0f : 9.0f);
    nvgFillColor(vg, nvgRGBA(7, 10, 12, prominent ? 224 : 198));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, 6.0f, box_height, 3.0f);
    nvgFillColor(vg, accent);
    nvgFill(vg);

    nvgFontSize(vg, prominent ? 22.0f : 21.0f);
    nvgFontFaceId(vg, opennow::ui::Font(opennow::ui::FontRole::Body));
    nvgFillColor(vg, nvgRGB(250, 252, 250));
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgText(vg, box_x + box_width * 0.5f, box_y + box_height * 0.5f, text.c_str(), nullptr);
}
