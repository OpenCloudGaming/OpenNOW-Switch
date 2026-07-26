#include "StreamView.hpp"
#include "network_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

void StreamView::DrawDebugOverlay(NVGcontext* vg, float x, float y, float width) {
    if (!session_)
        return;

    std::string debug_text = session_->get_debug_info();
    nvgFontSize(vg, 24.0f);
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
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

    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, nvgRGB(77, 218, 130));
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
    nvgFillColor(vg, nvgRGB(77, 218, 130));
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
            nvgFillColor(vg, nvgRGBA(77, 218, 130, 22 + static_cast<int>(pulse * 24.0f)));
            nvgFill(vg);
        }
        nvgBeginPath(vg);
        nvgCircle(vg, cx, rail_y, 22.0f);
        nvgFillColor(vg, complete || active ? nvgRGB(77, 218, 130) : nvgRGB(24, 28, 33));
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
    nvgStrokeColor(vg, nvgRGB(77, 218, 130));
    nvgStroke(vg);
    const float head_angle = angle + kPi * 1.42f;
    nvgBeginPath(vg);
    nvgCircle(vg, spinner_x + std::cos(head_angle) * 19.0f,
              spinner_y + std::sin(head_angle) * 19.0f, 3.4f);
    nvgFillColor(vg, nvgRGB(123, 242, 166));
    nvgFill(vg);

    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 20.0f);
    nvgFillColor(vg, nvgRGB(238, 242, 245));
    nvgText(vg, spinner_x, panel_y + 330.0f, title.c_str(), nullptr);
    nvgFontSize(vg, 15.0f);
    nvgFillColor(vg, nvgRGB(145, 153, 164));
    nvgText(vg, spinner_x, panel_y + 361.0f, detail.c_str(), nullptr);
}

void StreamView::UpdatePerformanceCounter(const VideoPerformanceCounters& counters) {
    const auto now = std::chrono::steady_clock::now();
    if (fps_window_started_.time_since_epoch().count() == 0) {
        fps_window_started_ = now;
        previous_video_counters_ = counters;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - fps_window_started_);
    if (elapsed >= std::chrono::milliseconds(750)) {
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
    nvgFillColor(vg, nvgRGB(55, 220, 125));
    nvgFill(vg);

    nvgFontSize(vg, 18.0f);
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
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
    gamepad_state_initialized_ = false;
    plus_was_down_ = false;
    plus_long_press_ = false;
    start_pulse_ = {};

    if (touch_was_down_ && session_)
    {
        session_->send_mouse_left_button(false);
        touch_was_down_ = false;
    }
    if (session_)
    {
        session_->send_gamepad_input(0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f);
        session_->record_ui_event(
            visible ? "stream overlay opened by Minus+Plus"
                    : "stream overlay closed");
    }

    if (!visible)
        keyboard_release_guard_ = true;
}

void StreamView::DrawStreamOverlay(
    NVGcontext* vg, float x, float y, float width, float height,
    std::chrono::steady_clock::time_point now)
{
    if (!stream_overlay_visible_)
        return;

    const VideoPerformanceCounters counters = previous_video_counters_;
    const StreamTransportHealth health =
        session_ ? session_->get_transport_health() : StreamTransportHealth {};
    const StreamNetworkCounters network = network_counters_;

    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillColor(vg, nvgRGBA(2, 5, 7, 178));
    nvgFill(vg);

    const float panel_x = x + 34.0f;
    const float panel_y = y + 28.0f;
    const float panel_width = width - 68.0f;
    const float panel_height = height - 56.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, panel_x, panel_y, panel_width, panel_height, 20.0f);
    nvgFillColor(vg, nvgRGBA(13, 17, 20, 246));
    nvgFill(vg);
    nvgStrokeWidth(vg, 1.5f);
    nvgStrokeColor(vg, nvgRGBA(90, 105, 112, 100));
    nvgStroke(vg);

    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 13.0f);
    nvgFillColor(vg, nvgRGB(77, 218, 130));
    nvgText(vg, panel_x + 30.0f, panel_y + 28.0f, "OPENNOW  /  IN-STREAM MENU", nullptr);
    nvgFontSize(vg, 26.0f);
    nvgFillColor(vg, nvgRGB(244, 248, 246));
    nvgText(vg, panel_x + 30.0f, panel_y + 62.0f,
            game_title_.empty() ? "GeForce NOW session" : game_title_.c_str(), nullptr);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, nvgRGB(142, 153, 160));
    const std::string provider =
        auth_.provider.display_name.empty() ? "GeForce NOW" : auth_.provider.display_name;
    nvgText(vg, panel_x + 30.0f, panel_y + 88.0f,
            (provider + "  |  B Close  |  Minus + Plus Toggle menu").c_str(), nullptr);

    const float divider_x = panel_x + panel_width * 0.56f;
    nvgBeginPath(vg);
    nvgRect(vg, divider_x, panel_y + 112.0f, 1.0f, panel_height - 140.0f);
    nvgFillColor(vg, nvgRGBA(94, 106, 112, 92));
    nvgFill(vg);

    const float left_x = panel_x + 30.0f;
    const float right_x = divider_x + 28.0f;
    float left_y = panel_y + 132.0f;
    float right_y = left_y;
    nvgFontSize(vg, 18.0f);
    nvgFillColor(vg, nvgRGB(239, 244, 241));
    nvgText(vg, left_x, left_y, "STREAM STATISTICS", nullptr);
    nvgText(vg, right_x, right_y, "CONTROLLER SHORTCUTS", nullptr);
    left_y += 34.0f;
    right_y += 34.0f;

    auto draw_row = [vg](float row_x, float& row_y, float value_x,
                         const std::string& label, const std::string& value) {
        nvgFontSize(vg, 15.0f);
        nvgFillColor(vg, nvgRGB(139, 150, 157));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(vg, row_x, row_y, label.c_str(), nullptr);
        nvgFillColor(vg, nvgRGB(238, 244, 240));
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(vg, value_x, row_y, value.c_str(), nullptr);
        row_y += 31.0f;
    };

    char number[96];
    const std::string resolution = session_
        ? std::to_string(session_->stream_width()) + " x " +
              std::to_string(session_->stream_height())
        : "--";
    draw_row(left_x, left_y, divider_x - 28.0f, "Resolution", resolution);
    std::snprintf(number, sizeof(number), "%.0f / %.0f / %.0f",
                  incoming_fps_, decoded_fps_, presented_fps_);
    draw_row(left_x, left_y, divider_x - 28.0f, "FPS  in / decode / display", number);
    std::snprintf(number, sizeof(number), "%.1f Mbps", stream_bitrate_mbps_);
    draw_row(left_x, left_y, divider_x - 28.0f, "Bitrate", number);
    draw_row(left_x, left_y, divider_x - 28.0f, "Network latency",
             network_rtt_ms_ >= 0 ? std::to_string(network_rtt_ms_) + " ms" : "-- ms");
    const std::uint64_t packet_total =
        static_cast<std::uint64_t>(network.packets_received) + network.sequence_gaps;
    const double packet_loss = packet_total > 0
        ? static_cast<double>(network.sequence_gaps) * 100.0 /
              static_cast<double>(packet_total)
        : 0.0;
    std::snprintf(number, sizeof(number), "%.2f%%  (%u gaps / %u late)",
                  packet_loss, network.sequence_gaps, network.late_packets_dropped);
    draw_row(left_x, left_y, divider_x - 28.0f, "Packet loss", number);
    draw_row(left_x, left_y, divider_x - 28.0f, "Dropped video frames",
             std::to_string(network.access_units_dropped));
    draw_row(left_x, left_y, divider_x - 28.0f, "NACK recovery requests",
             std::to_string(network.nack_requests));
    draw_row(left_x, left_y, divider_x - 28.0f, "Decode latency p95",
             std::to_string(counters.decode_us_p95 / 1000) + " ms");
    draw_row(left_x, left_y, divider_x - 28.0f, "Render latency p95",
             std::to_string(counters.render_us_p95 / 1000) + " ms");
    draw_row(left_x, left_y, divider_x - 28.0f, "Decoder queue",
             std::to_string(counters.decode_queue_size) + " / " +
                 std::to_string(counters.decode_queue_high_water) + " high");
    draw_row(left_x, left_y, divider_x - 28.0f, "Connection",
             health.peer_completed ? "Connected" : "Negotiating");
    draw_row(left_x, left_y, divider_x - 28.0f, "Codec / location",
             stream_codec_ + "  /  " + stream_region_);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - stream_started_at_).count();
    std::snprintf(number, sizeof(number), "%lld:%02lld",
                  static_cast<long long>(elapsed / 60),
                  static_cast<long long>(elapsed % 60));
    draw_row(left_x, left_y, divider_x - 28.0f, "Session time", number);

    auto draw_shortcut = [vg, right_x](
                             float& row_y, const char* keys, const char* action) {
        const float key_width = 126.0f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, right_x, row_y - 13.0f, key_width, 28.0f, 6.0f);
        nvgFillColor(vg, nvgRGBA(39, 47, 51, 230));
        nvgFill(vg);
        nvgStrokeWidth(vg, 1.0f);
        nvgStrokeColor(vg, nvgRGBA(113, 129, 137, 130));
        nvgStroke(vg);
        nvgFontSize(vg, 14.0f);
        nvgFillColor(vg, nvgRGB(240, 245, 242));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, right_x + key_width * 0.5f, row_y + 1.0f, keys, nullptr);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, nvgRGB(188, 198, 202));
        nvgText(vg, right_x + key_width + 18.0f, row_y + 1.0f, action, nullptr);
        row_y += 42.0f;
    };

    draw_shortcut(right_y, "Minus + Plus", "Open / close this menu");
    draw_shortcut(right_y, "Minus + Y", "On-screen keyboard");
    draw_shortcut(right_y, "B", "Close menu or keyboard");
    draw_shortcut(right_y, "ZL + ZR + Minus", "Exit the stream");
    draw_shortcut(right_y, "Hold +", "Xbox Guide button");
    draw_shortcut(right_y, "Touch", "Remote pointer and click");
    if (is_nte_session_)
        draw_shortcut(right_y, "L + X", "NTE auto-login");

    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 13.0f);
    nvgFillColor(vg, nvgRGB(103, 116, 122));
    nvgText(vg, right_x, panel_y + panel_height - 26.0f,
            "Menu input stays local and is never sent to the cloud game.", nullptr);
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
    nvgFillColor(vg, nvgRGB(55, 220, 125));
    nvgFill(vg);
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
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

    if (last_network_check_at_.time_since_epoch().count() == 0 ||
        now - last_network_check_at_ >= std::chrono::seconds(1)) {
        internet_connected_ = opennow::NetworkUtils::HasInternetConnection();
        last_network_check_at_ = now;
    }

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
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
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
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgFillColor(vg, nvgRGB(250, 252, 250));
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgText(vg, box_x + box_width * 0.5f, box_y + box_height * 0.5f, text.c_str(), nullptr);
}
