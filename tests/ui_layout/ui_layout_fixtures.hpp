#pragma once

#include "home_shortcut.hpp"
#include "models.hpp"
#include "stream_settings.hpp"

#include <string>
#include <vector>
#include <array>

namespace opennow { class CachedImage; }

namespace ui_fixture
{
struct LaunchCall { std::string user_id, app_id, title, store; };
struct CoverCall { const opennow::CachedImage* image; std::string url; };
struct TextDraw
{
    std::string text;
    std::array<float, 4> bounds;
    std::array<float, 6> transform;
    float x, y, raw_x, raw_y;
    bool scrolling_path;
};
struct Calls
{
    int library_requests = 0;
    int catalog_requests = 0;
    int public_requests = 0;
    int polls = 0;
    int region_requests = 0;
    int latency_measurements = 0;
    std::vector<std::string> search_queries, cursors;
    std::vector<LaunchCall> launches;
    std::vector<opennow::shortcut::LaunchRequest> shortcuts;
    std::vector<std::string> saved_variants, cover_urls, stopped_sessions;
    std::vector<CoverCall> cover_requests;
    std::vector<std::string> handed_off_sessions, played_ids, notifications;
    std::vector<opennow::StreamSettings> start_settings, handoff_settings;
};
enum class SessionMode { Ready, Waiting, BlockStart, Unknown, Patching, Confirmation, Failure };
void Reset(std::string storage_directory, bool empty);
Calls Snapshot();
std::vector<opennow::GameInfo> Library();
std::vector<opennow::PublicGame> Catalog();
void SetSessionMode(SessionMode mode);
void ReleaseStart();
void ClearLauncherPreference();
void RecordNotification(const std::string& text);
void BeginTextCapture();
std::vector<TextDraw> EndTextCapture();
}
