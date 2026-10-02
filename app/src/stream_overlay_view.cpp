#include "stream_overlay_view.hpp"
#include "localization.hpp"
#include "ui_theme.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace opennow
{

StreamOverlayDisplay FormatStreamOverlay(const StreamOverlaySample& sample)
{
    StreamOverlayDisplay display;
    display.game_title = sample.game_title.empty() ? Tr("GeForce NOW session") : sample.game_title;
    display.provider = sample.provider.empty() ? "GeForce NOW" : sample.provider;
    display.connection = Tr(sample.peer_connected ? "Connected" : sample.signaling_connected ? "Negotiating" : "Connecting");
    display.peer_connected = sample.peer_connected;
    display.wifi_warning = sample.wifi_warning;
    display.nte_session = sample.nte_session;
    char number[96];
    std::snprintf(number, sizeof(number), "%.0f", sample.presented_fps);
    display.metrics[0] = number;
    std::snprintf(number, sizeof(number), "%.1f", sample.bitrate_mbps);
    display.metrics[1] = number;
    display.metrics[2] = sample.rtt_ms < 0 ? Tr("Unknown") : std::to_string(sample.rtt_ms);
    const auto packets = sample.packets_received + sample.sequence_gaps;
    if (packets > 0) {
        std::snprintf(number, sizeof(number), "%.2f", static_cast<double>(sample.sequence_gaps) * 100.0 / static_cast<double>(packets));
        display.metrics[3] = number;
    } else {
        display.metrics[3] = Tr("Unknown");
    }
    display.details[0] = sample.width > 0 && sample.height > 0
        ? std::to_string(sample.width) + "×" + std::to_string(sample.height) : Tr("Unknown");
    std::snprintf(number, sizeof(number), "%.0f / %.0f / %.0f", sample.incoming_fps, sample.decoded_fps, sample.presented_fps);
    display.details[1] = number;
    std::snprintf(number, sizeof(number), "%.1f ms", static_cast<double>(sample.decode_us_p95) / 1000.0);
    display.details[2] = number;
    std::snprintf(number, sizeof(number), "%.1f ms", static_cast<double>(sample.render_us_p95) / 1000.0);
    display.details[3] = number;
    display.details[4] = std::to_string(sample.queue_size) + " / " + std::to_string(sample.queue_high_water);
    display.details[5] = sample.network.empty() ? Tr("Unknown") : sample.network;
    display.details[6] = (sample.codec.empty() ? Tr("Unknown") : sample.codec) + " / " + (sample.location.empty() ? Tr("Auto") : sample.location);
    display.details[7] = std::to_string(sample.dropped_frames);
    display.details[8] = std::to_string(sample.nack_requests);
    display.details[9] = std::to_string(sample.late_packets_dropped);
    return display;
}

void SetStreamOverlayElapsed(StreamOverlayDisplay& display, std::uint64_t seconds)
{
    char elapsed[48];
    std::snprintf(elapsed, sizeof(elapsed), "%llu:%02llu", static_cast<unsigned long long>(seconds / 60), static_cast<unsigned long long>(seconds % 60));
    display.elapsed = elapsed;
}

StreamOverlayView::StreamOverlayView()
{
    setId("stream-overlay/root");
    setFocusable(true);
    setHideHighlightBackground(true);
    setHideHighlightBorder(true);
    logo_.setImageFromRes("img/opennow-logo-mark.png");
    for (size_t index = 0; index < fonts_.size(); ++index)
        fonts_[index] = ui::Font(static_cast<ui::FontRole>(index));
}

void StreamOverlayView::Update(const StreamOverlaySample& sample)
{
    std::string elapsed = std::move(display_.elapsed);
    display_ = FormatStreamOverlay(sample);
    display_.elapsed = std::move(elapsed);
}

void StreamOverlayView::UpdateElapsed(std::uint64_t seconds)
{
    SetStreamOverlayElapsed(display_, seconds);
}

void StreamOverlayView::draw(NVGcontext* vg, float x, float y, float width, float height,
                             brls::Style, brls::FrameContext*)
{
    nvgSave(vg);
    nvgTranslate(vg, x, y);
    nvgScale(vg, width / 1280.0f, height / 720.0f);
    auto panel = [vg](float px, float py, float w, float h) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, px, py, w, h, 12);
        nvgFillColor(vg, ui::Raised());
        nvgFill(vg);
        nvgStrokeWidth(vg, 1);
        nvgStrokeColor(vg, ui::Rule());
        nvgStroke(vg);
    };
    auto text = [vg, this](float tx, float ty, const char* copy, float size, NVGcolor color,
                     ui::FontRole font = ui::FontRole::Body, int align = NVG_ALIGN_LEFT) {
        nvgFontFaceId(vg, fonts_[static_cast<size_t>(font)]);
        nvgFontSize(vg, size);
        nvgFillColor(vg, color);
        nvgTextAlign(vg, align | NVG_ALIGN_MIDDLE);
        nvgText(vg, tx, ty, copy, nullptr);
    };
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, 1280, 720);
    nvgFillColor(vg, nvgRGBA(11, 12, 14, 242));
    nvgFill(vg);
    if (logo_.getTexture() > 0) {
        const float mark_height = 48.0f * logo_.getOriginalImageHeight() / logo_.getOriginalImageWidth();
        const float mark_y = 44.0f - mark_height * 0.5f;
        nvgBeginPath(vg);
        nvgRect(vg, 40, mark_y, 48, mark_height);
        nvgFillPaint(vg, nvgImagePattern(vg, 40, mark_y, 48, mark_height, 0, logo_.getTexture(), 1));
        nvgFill(vg);
    }
    text(98, 44, "OpenNOW · Stream status", 15, ui::Muted(), ui::FontRole::Medium);
    nvgFontFaceId(vg, fonts_[static_cast<size_t>(ui::FontRole::Heading)]);
    nvgFontSize(vg, 29);
    const bool long_title = nvgTextBounds(vg, 0, 0, display_.game_title.c_str(), nullptr, nullptr) > 860;
    nvgSave(vg);
    nvgScissor(vg, 40, 66, long_title ? 818 : 860, 48);
    text(40, 88, display_.game_title.c_str(), 29, ui::Text(), ui::FontRole::Heading);
    nvgRestore(vg);
    if (long_title)
        text(870, 88, "…", 29, ui::Text(), ui::FontRole::Heading);
    panel(940, 70, 300, 36);
    text(1090, 88, display_.connection.c_str(), 17, display_.peer_connected ? ui::Green() : ui::Muted(), ui::FontRole::Medium, NVG_ALIGN_CENTER);
    nvgSave(vg);
    nvgScissor(vg, 40, 108, 1100, 28);
    text(40, 124, display_.provider.c_str(), 17, ui::Muted());
    nvgRestore(vg);
    text(1240, 124, display_.elapsed.c_str(), 17, ui::Muted(), ui::FontRole::Medium, NVG_ALIGN_RIGHT);

    static constexpr const char* metric_labels[] = {"Display FPS", "Bitrate", "Ping", "Packet loss"};
    static constexpr const char* units[] = {"fps", "Mbps", "ms", "%"};
    for (size_t index = 0; index < 4; ++index)
    {
        const float mx = 40 + static_cast<float>(index) * 303.75f;
        panel(mx, 160, 288.75f, 116);
        text(mx + 18, 185, metric_labels[index], 16, ui::Muted());
        text(mx + 18, 224, display_.metrics[index].c_str(), display_.metrics[index].size() > 6 ? 29 : 38, ui::Text(), ui::FontRole::Display);
        nvgFontFaceId(vg, fonts_[static_cast<size_t>(ui::FontRole::Display)]);
        nvgFontSize(vg, display_.metrics[index].size() > 6 ? 29 : 38);
        const float value_width = nvgTextBounds(vg, 0, 0, display_.metrics[index].c_str(), nullptr, nullptr);
        text(mx + 26 + value_width, 228, units[index], 16, ui::Muted());
        text(mx + 18, 254, index == 0 ? "Presented frames" : index == 1 ? "Incoming video" : index == 2 ? "Round-trip time" : "Sequence gaps / total", 13, ui::Muted());
    }
    panel(40, 296, 730, 344);
    panel(794, 296, 446, 344);
    text(60, 324, "Stream details", 18, ui::Text(), ui::FontRole::Heading);
    text(814, 324, "Controller shortcuts", 18, ui::Text(), ui::FontRole::Heading);
    static constexpr const char* detail_labels[] = {
        "Resolution", "FPS in / decode / display", "Decode latency p95", "Render latency p95", "Decoder queue / high-water",
        "Network", "Codec / configured location", "Dropped video frames", "NACK recovery requests", "Late packets dropped"};
    for (size_t index = 0; index < 10; ++index)
    {
        const size_t column = index / 5;
        const float dx = 60 + static_cast<float>(column) * 354;
        const float dy = 366 + static_cast<float>(index % 5) * 48;
        text(dx, dy, detail_labels[index], 14, ui::Muted());
        nvgSave(vg);
        nvgScissor(vg, dx, dy + 8, 330, 24);
        text(dx, dy + 24, display_.details[index].c_str(), 16, ui::Text(), ui::FontRole::Mono);
        nvgRestore(vg);
    }
    static constexpr const char* keys[] = {"Minus + Plus", "Minus + Y", "Keyboard strip", "B", "ZL + ZR + −", "Hold +", "Touch", "L + X"};
    static constexpr const char* actions[] = {"Open or close this menu", "On-screen keyboard", "Esc, Win and Windows shortcuts", "Close menu or keyboard", "Exit the stream", "Xbox Guide button", "Remote pointer", "NTE auto-login"};
    for (size_t index = 0; index < (display_.nte_session ? 8u : 7u); ++index)
    {
        const float sy = 358 + static_cast<float>(index) * 32;
        panel(814, sy - 12, 120, 28);
        text(874, sy + 2, keys[index], 14, ui::Text(), ui::FontRole::Medium, NVG_ALIGN_CENTER);
        text(948, sy + 2, actions[index], 16, ui::Muted());
    }
    if (display_.wifi_warning)
        text(814, 615, "2.4 GHz Wi-Fi · Use 5 GHz or Ethernet", 14, ui::Danger(), ui::FontRole::Medium);
    nvgBeginPath(vg);
    nvgRect(vg, 0, 660, 1280, 1);
    nvgFillColor(vg, ui::Rule());
    nvgFill(vg);
    text(40, 690, "Menu controls stay on your Switch and are not sent to the game.", 16, ui::Muted());
    text(1240, 690, "B  Close menu", 17, ui::Text(), ui::FontRole::Medium, NVG_ALIGN_RIGHT);
    nvgRestore(vg);
}

}
