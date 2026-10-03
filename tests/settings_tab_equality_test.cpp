#include "stream_settings.hpp"

#include <cassert>

int main()
{
    const opennow::StreamSettings saved;
    assert(saved == saved);

    const auto check = [&saved](auto member, auto replacement) {
        auto draft = saved;
        draft.*member = replacement;
        assert(draft != saved);
        assert(saved != draft);
        draft.*member = saved.*member;
        assert(draft == saved);
    };
    using Settings = opennow::StreamSettings;
    check(&Settings::preset_id, "custom");
    check(&Settings::label, "Custom");
    check(&Settings::width, 1920);
    check(&Settings::height, 1080);
    check(&Settings::fps, 30);
    check(&Settings::bitrate_kbps, 12500);
    check(&Settings::codec, "H265");
    check(&Settings::region, "https://example.invalid/region");
    check(&Settings::audio_enabled, false);
    check(&Settings::audio_volume, 1000);
    check(&Settings::audio_buffer_ms, 80);
    check(&Settings::video_backend, "Software");
    check(&Settings::debug_diagnostics, true);
    check(&Settings::stats_overlay_enabled, true);
    check(&Settings::game_language, "fr_FR");
    check(&Settings::persist_game_settings, false);
    check(&Settings::controller_layout, "Switch");
    check(&Settings::image_quality_mode, "Original");
    check(&Settings::interface_language, "zh-CN");
    check(&Settings::community_proxy_enabled, true);
    check(&Settings::community_proxy_url, "https://example.invalid/proxy");
    for (int threshold : {5, 20, 50})
        check(&Settings::queue_notify_threshold, threshold);
}
