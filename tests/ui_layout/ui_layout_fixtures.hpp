#pragma once

#include "home_shortcut.hpp"
#include "models.hpp"
#include "stream_settings.hpp"

#include <string>
#include <vector>

namespace ui_fixture
{
struct LaunchCall { std::string user_id, app_id, title, store; };
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
}
