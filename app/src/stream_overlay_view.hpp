#pragma once

#include <borealis.hpp>
#include <array>
#include <cstdint>
#include <string>

namespace opennow
{

struct StreamOverlaySample
{
    std::string game_title;
    std::string provider;
    std::string codec;
    std::string location;
    std::string network;
    float incoming_fps = 0;
    float decoded_fps = 0;
    float presented_fps = 0;
    float bitrate_mbps = 0;
    int rtt_ms = -1;
    int width = 0;
    int height = 0;
    std::uint64_t decode_us_p95 = 0;
    std::uint64_t render_us_p95 = 0;
    std::uint64_t queue_size = 0;
    std::uint64_t queue_high_water = 0;
    std::uint64_t packets_received = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t late_packets_dropped = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t nack_requests = 0;
    bool peer_connected = false;
    bool signaling_connected = false;
    bool wifi_warning = false;
    bool nte_session = false;
};

struct StreamOverlayDisplay
{
    std::string game_title;
    std::string provider;
    std::string connection;
    std::array<std::string, 4> metrics;
    std::array<std::string, 10> details;
    std::string elapsed;
    bool peer_connected = false;
    bool wifi_warning = false;
    bool nte_session = false;
};

StreamOverlayDisplay FormatStreamOverlay(const StreamOverlaySample& sample);
void SetStreamOverlayElapsed(StreamOverlayDisplay& display, std::uint64_t seconds);

class StreamOverlayView final : public brls::View
{
  public:
    StreamOverlayView();
    void Update(const StreamOverlaySample& sample);
    void UpdateElapsed(std::uint64_t seconds);
    const StreamOverlayDisplay& Display() const { return display_; }
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

  private:
    brls::Image logo_;
    std::array<int, 5> fonts_ {};
    StreamOverlayDisplay display_;
};

}
