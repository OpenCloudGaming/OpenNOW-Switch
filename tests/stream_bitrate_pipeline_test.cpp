#include "app_paths.hpp"
#include "gfn/cloud_session_internal.hpp"
#include "stream_settings_policy.hpp"
#include "webrtc/nvst_sdp.hpp"

#include <jansson.h>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace
{

std::string test_home;

void CheckNegotiatedSettings(const opennow::StreamSettings& settings)
{
    const std::string body = opennow::gfn::cloud_session::BuildSessionBody(
        "123", "test-title", "test-device", "test-sub-session", "", settings);
    json_error_t error {};
    std::unique_ptr<json_t, decltype(&json_decref)> root(
        json_loads(body.c_str(), 0, &error), &json_decref);
    assert(root);
    json_t* request = json_object_get(root.get(), "sessionRequestData");
    json_t* features = json_object_get(request, "requestedStreamingFeatures");
    assert(json_integer_value(json_object_get(features, "maxBitrateKbps")) ==
           settings.bitrate_kbps);
    json_t* monitor = json_array_get(
        json_object_get(request, "clientRequestMonitorSettings"), 0);
    assert(json_integer_value(json_object_get(monitor, "widthInPixels")) == settings.width);
    assert(json_integer_value(json_object_get(monitor, "heightInPixels")) == settings.height);
    assert(json_integer_value(json_object_get(monitor, "framesPerSecond")) == settings.fps);

    const std::string sdp = opennow::webrtc::BuildNvstSdp(
        "v=0\r\na=ice-ufrag:test\r\n", settings, {});
    const auto has_line = [&sdp](const std::string& line) {
        return sdp.find("\n" + line + "\n") != std::string::npos;
    };
    assert(has_line("a=vqos.bw.maximumBitrateKbps:" + std::to_string(settings.bitrate_kbps)));
    assert(has_line("a=vqos.bw.minimumBitrateKbps:4000"));
    const std::string startup = std::to_string(std::max(4000, settings.bitrate_kbps / 4));
    assert(has_line("a=video.initialBitrateKbps:" + startup));
    assert(has_line("a=video.initialPeakBitrateKbps:" + startup));
    assert(has_line("a=video.maxFPS:" + std::to_string(settings.fps)));
}

void CheckSavedSettings(const opennow::StreamSettings& settings)
{
    assert(opennow::SaveStreamSettings(settings));
    const auto loaded = opennow::LoadStreamSettings();
    assert(loaded.bitrate_kbps == settings.bitrate_kbps);
    assert(loaded.width == settings.width);
    assert(loaded.height == settings.height);
    assert(loaded.fps == settings.fps);
    assert(loaded.image_quality_mode == settings.image_quality_mode);
    assert(loaded.preset_id == settings.preset_id);
    assert(!std::filesystem::exists(test_home + "/stream_settings.json.tmp"));
    CheckNegotiatedSettings(loaded);
}

}

namespace opennow
{

const std::string& AppHomePath()
{
    return test_home;
}

void PrepareAppStorage()
{
    std::filesystem::create_directories(test_home);
}

}

int main()
{
    std::string directory =
        (std::filesystem::temp_directory_path() / "opennow-bitrate-test-XXXXXX").string();
    assert(mkdtemp(directory.data()));
    test_home = directory;

    assert(opennow::LoadStreamSettings().bitrate_kbps == 12000);
    for (const auto& preset : opennow::StreamPresets())
        CheckSavedSettings(preset);

    opennow::StreamSettings settings;
    settings.bitrate_kbps = 25000;
    for (int bitrate : {8000, 12000, 16000, 20000, 25000})
    {
        opennow::settings::CycleBitrate(settings);
        assert(settings.bitrate_kbps == bitrate);
        assert(settings.preset_id == "custom");
        for (int fps : {30, 60})
        {
            settings.fps = fps;
            for (const char* quality : {"Original", "Adaptive", "Clarity"})
            {
                settings.image_quality_mode = quality;
                CheckSavedSettings(settings);
            }
        }
    }

    settings.bitrate_kbps = 17500;
    CheckSavedSettings(settings);
    const auto active_session_settings = opennow::LoadStreamSettings();
    settings.bitrate_kbps = 25000;
    CheckSavedSettings(settings);
    assert(active_session_settings.bitrate_kbps == 17500);
    CheckNegotiatedSettings(active_session_settings);

    {
        std::ofstream legacy(test_home + "/stream_settings.json");
        legacy << R"({"preset_id":"custom","width":1280,"height":720,"fps":60})";
    }
    assert(opennow::LoadStreamSettings().bitrate_kbps == 12000);
    CheckNegotiatedSettings(opennow::LoadStreamSettings());

    std::filesystem::remove_all(test_home);
    return 0;
}
