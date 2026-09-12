#include "app_paths.hpp"
#include "stream_settings.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace
{

std::string home;
int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void Write(const std::string& suffix, const std::string& body)
{
    std::ofstream file(home + "/stream_settings.json" + suffix);
    file << body;
    file.close();
    assert(file.good());
}

}

namespace opennow
{

const std::string& AppHomePath()
{
    return home;
}

void PrepareAppStorage()
{
    std::filesystem::create_directories(home);
}

}

int main()
{
    std::string pattern =
        (std::filesystem::temp_directory_path() / "opennow-settings-XXXXXX").string();
    char* directory = mkdtemp(pattern.data());
    assert(directory);
    home = directory;

    Check(opennow::LoadStreamSettings().fps == 60, "missing settings use balanced defaults");
    Write(".bak", R"({"fps":30,"interface_language":"ru","audio_enabled":false})");
    auto settings = opennow::LoadStreamSettings();
    Check(settings.fps == 30 && settings.interface_language == "ru" && !settings.audio_enabled,
          "missing primary recovers the backup");
    for (const std::string body : {"", "{", "[]", "null"})
    {
        Write("", body);
        Check(opennow::LoadStreamSettings().fps == 30,
              "unreadable primary recovers the backup object");
    }

    Write("", R"({"fps":60})");
    Check(opennow::LoadStreamSettings().fps == 60, "valid primary wins over backup");
    Write(".bak", "{");
    Write("", "{");
    Check(opennow::LoadStreamSettings().fps == 60, "invalid primary and backup use defaults");

    Write("", R"({"width":0,"fps":30,"bitrate_kbps":20000,"interface_language":"ru","controller_layout":"Switch","audio_enabled":false,"stats_overlay_enabled":true})");
    settings = opennow::LoadStreamSettings();
    Check(settings.width == 1280 && settings.fps == 30 && settings.bitrate_kbps == 20000,
          "invalid width preserves valid video fields");
    Check(settings.interface_language == "ru" && settings.controller_layout == "Switch" &&
              !settings.audio_enabled && settings.stats_overlay_enabled,
          "invalid video preserves unrelated preferences");

    Write("", R"({"width":4294969216,"height":4294968376,"fps":4294967326,"bitrate_kbps":4294975296,"audio_volume":4294968896,"audio_gain_version":4})");
    settings = opennow::LoadStreamSettings();
    Check(settings.width == 1280 && settings.height == 720 && settings.fps == 60 &&
              settings.bitrate_kbps == 12000 && settings.audio_volume == 1200,
          "out-of-range JSON integers do not wrap into valid settings");

    Write("", R"({"fps":-30,"height":-720,"bitrate_kbps":0,"audio_gain_version":4,"audio_volume":1600})");
    settings = opennow::LoadStreamSettings();
    Check(settings.fps == 60 && settings.height == 720 && settings.bitrate_kbps == 12000 &&
              settings.audio_volume == 1600,
          "nonpositive video fields use individual defaults");

    for (const auto& [body, volume] : {
             std::pair{R"({"audio_volume":150})", 1200},
             std::pair{R"({"audio_volume":600,"audio_gain_version":3})", 1200},
             std::pair{R"({"audio_volume":-2147483648,"audio_gain_version":3})", 800},
             std::pair{R"({"audio_volume":800,"audio_gain_version":4})", 800},
             std::pair{R"({"audio_enabled":false})", 1200}})
    {
        Write("", body);
        Check(opennow::LoadStreamSettings().audio_volume == volume,
              "legacy audio gain migration remains compatible");
    }

    settings.interface_language = "ru";
    settings.controller_layout = "Switch";
    settings.audio_enabled = false;
    settings.width = 0;
    Check(opennow::SaveStreamSettings(settings), "settings save succeeds");
    settings = opennow::LoadStreamSettings();
    Check(settings.width == 1280 && settings.interface_language == "ru" &&
              settings.controller_layout == "Switch" && !settings.audio_enabled,
          "save normalization preserves unrelated preferences");
    Check(!std::filesystem::exists(home + "/stream_settings.json.tmp"),
          "successful save leaves no temporary file");

    for (const auto& preset : opennow::StreamPresets())
    {
        Check(opennow::SaveStreamSettings(preset), "each existing preset saves");
        const auto loaded = opennow::LoadStreamSettings();
        Check(loaded.preset_id == preset.preset_id && loaded.label == preset.label &&
                  loaded.width == preset.width && loaded.height == preset.height &&
                  loaded.fps == preset.fps && loaded.bitrate_kbps == preset.bitrate_kbps,
              "existing preset video parameters round trip unchanged");
    }

    Write("", R"({"fps":"30","audio_volume":true,"audio_enabled":0,"audio_gain_version":4})");
    settings = opennow::LoadStreamSettings();
    Check(settings.fps == 60 && settings.audio_volume == 1200 && settings.audio_enabled,
          "wrong JSON types keep defaults");

    std::filesystem::remove_all(home);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
