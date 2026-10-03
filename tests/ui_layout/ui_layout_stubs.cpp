#include "ui_layout_fixtures.hpp"
#include "app_paths.hpp"
#include "avatar_utils.hpp"
#include "cover_image_cache.hpp"
#include "gfn_client.hpp"
#include "cloud_launch_internal.hpp"
#include "network_utils.hpp"
#include "nte_credentials.hpp"
#include "play_history.hpp"
#include "ui_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>

namespace
{
std::mutex fixture_mutex;
ui_fixture::Calls calls;
std::vector<opennow::GameInfo> library;
std::vector<opennow::PublicGame> catalog;
std::string storage, saved_variant;
ui_fixture::SessionMode session_mode = ui_fixture::SessionMode::Ready;
std::condition_variable start_gate;
bool start_released = false;
bool capture_text = false;
std::vector<ui_fixture::TextDraw> text_draws;

void RecordTextDraw(NVGcontext* vg, float x, float y, const char* text, const char* end, bool scrolling_path)
{
    if (!capture_text || !text || text == end || !*text) return;
    float bounds[4] {}, transform[6] {};
    nvgTextBounds(vg, x, y, text, end, bounds);
    nvgCurrentTransform(vg, transform);
    ui_fixture::TextDraw draw;
    draw.text = end ? std::string(text, end) : std::string(text);
    std::copy(std::begin(transform), std::end(transform), draw.transform.begin());
    draw.raw_x = x;
    draw.raw_y = y;
    const float scale = brls::Application::windowScale;
    nvgTransformPoint(&draw.bounds[0], &draw.bounds[1], transform, bounds[0], bounds[1]);
    nvgTransformPoint(&draw.bounds[2], &draw.bounds[3], transform, bounds[2], bounds[3]);
    nvgTransformPoint(&draw.x, &draw.y, transform, x, y);
    for (auto& value : draw.bounds) value /= scale;
    draw.x /= scale;
    draw.y /= scale;
    draw.scrolling_path = scrolling_path;
    text_draws.push_back(std::move(draw));
}
}

namespace ui_fixture
{
void Reset(std::string directory, bool empty)
{
    std::lock_guard lock(fixture_mutex);
    calls = {};
    storage = std::move(directory);
    std::filesystem::create_directories(storage);
    library.clear();
    catalog.clear();
    saved_variant.clear();
    session_mode = SessionMode::Ready;
    start_released = false;
    if (empty) return;
    for (int index = 0; index < 21; ++index)
    {
        opennow::GameInfo game;
        game.id = std::to_string(1000 + index);
        game.uuid = "fixture-" + game.id;
        game.launch_app_id = game.id;
        game.title = index == 0 ? "A very long game title for native clipping verification Жї 玩家" :
            std::string(1, static_cast<char>('B' + index)) + " Fixture game " + std::to_string(index);
        game.description = "A native fixture description with long wrapping text. "
            "This is test data at the external feed boundary, not a runtime catalog entry. "
            "Описание игры. 游戏描述。";
        const std::string paragraph = game.description;
        for (int line = 0; line < 12; ++line) game.description += "\n" + paragraph;
        game.publisher = index % 2 == 0 ? "Zulu publisher" : "Alpha publisher";
        game.available_stores = {index % 2 == 0 ? "Steam" : "Epic"};
        game.membership_tier_label = "Free";
        game.image_url = "fixture://cover/" + game.id;
        game.is_in_library = true;
        game.last_played = index == 0 ? "2026-10-02T12:00:00Z" :
            index == 1 ? "2026-09-29T12:00:00Z" : index == 2 ? "2026-09-01T12:00:00Z" : "";
        game.variants = {{game.id, game.available_stores.front(), "OWNED", game.last_played, "AVAILABLE", true}};
        library.push_back(game);
        opennow::PublicGame item;
        item.id = game.id;
        item.uuid = game.uuid;
        item.launch_app_id = game.id;
        item.title = game.title;
        item.publisher = game.publisher;
        item.store = game.available_stores.front();
        item.image_url = game.image_url;
        item.is_in_library = index % 3 == 0;
        item.variants = game.variants;
        catalog.push_back(std::move(item));
    }
}
Calls Snapshot() { std::lock_guard lock(fixture_mutex); return calls; }
std::vector<opennow::GameInfo> Library() { std::lock_guard lock(fixture_mutex); return library; }
std::vector<opennow::PublicGame> Catalog() { std::lock_guard lock(fixture_mutex); return catalog; }
void SetSessionMode(SessionMode mode) { std::lock_guard lock(fixture_mutex); session_mode = mode; start_released = false; }
void ReleaseStart() { { std::lock_guard lock(fixture_mutex); start_released = true; } start_gate.notify_all(); }
void ClearLauncherPreference() { std::lock_guard lock(fixture_mutex); saved_variant.clear(); }
void RecordNotification(const std::string& text) { std::lock_guard lock(fixture_mutex); calls.notifications.push_back(text); }
void BeginTextCapture() { text_draws.clear(); capture_text = true; }
std::vector<TextDraw> EndTextCapture() { capture_text = false; return std::move(text_draws); }
}

extern "C" float __real_nvgText(NVGcontext*, float, float, const char*, const char*);
extern "C" float __wrap_nvgText(NVGcontext* vg, float x, float y, const char* text, const char* end)
{
    RecordTextDraw(vg, x, y, text, end, true);
    return __real_nvgText(vg, x, y, text, end);
}
extern "C" float __real_nvgTextWithCursor(NVGcontext*, float, float, const char*, const char*, int);
extern "C" float __wrap_nvgTextWithCursor(NVGcontext* vg, float x, float y, const char* text, const char* end, int cursor)
{
    RecordTextDraw(vg, x, y, text, end, false);
    return __real_nvgTextWithCursor(vg, x, y, text, end, cursor);
}

extern "C" void RealNotify(const std::string& text)
    asm("__real__ZN4brls11Application6notifyERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
extern "C" void ObserveNotify(const std::string& text)
    asm("__wrap__ZN4brls11Application6notifyERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
extern "C" void ObserveNotify(const std::string& text)
{
    ui_fixture::RecordNotification(text);
    RealNotify(text);
}

namespace opennow
{
const std::string& AppHomePath() { return storage; }
const std::string& LegacyAppHomePath() { return storage; }
void PrepareAppStorage() { std::filesystem::create_directories(storage); }
CachedImage::~CachedImage() = default;
void CachedImage::draw(NVGcontext* vg, float x, float y, float width, float height,
                       brls::Style style, brls::FrameContext* ctx)
{ brls::Image::draw(vg, x, y, width, height, style, ctx); }
void SetCachedCoverImage(CachedImage* image, const std::string& url)
{
    {
        std::lock_guard lock(fixture_mutex);
        calls.cover_urls.push_back(url);
        calls.cover_requests.push_back({image, url});
    }
    image->setImageFromRes("img/opennow-logo-mark.png");
}
void SetCachedAvatarImage(CachedImage*, const std::string&) {}
void CachedImage::SetUrl(const std::string& url, bool) { SetCachedCoverImage(this, url); }
CoverImageCacheStats InspectCoverImageCache() { return {4096, 2}; }
std::size_t ClearCoverImageCache() { return 2; }
void ShutdownCoverImageWorker() {}
std::string ResolveAvatarUrl(const AuthUser&) { return {}; }
GfnClient::GfnClient() = default;
std::vector<GameInfo> GfnClient::FetchLibraryGames(AuthSession&) const
{ std::lock_guard lock(fixture_mutex); ++calls.library_requests; return library; }
CatalogPage GfnClient::FetchCatalogPage(AuthSession&, const std::string& query, const std::string& cursor) const
{
    std::lock_guard lock(fixture_mutex);
    ++calls.catalog_requests;
    calls.search_queries.push_back(query);
    calls.cursors.push_back(cursor);
    std::vector<PublicGame> matches;
    for (const auto& game : catalog)
        if (query.empty() || game.title.find(query) != std::string::npos) matches.push_back(game);
    const size_t start = cursor.empty() ? 0 : 15;
    const size_t end = std::min(matches.size(), start + 15);
    CatalogPage page;
    if (start < matches.size()) page.games.assign(matches.begin() + start, matches.begin() + end);
    page.total_count = matches.size();
    if (end < matches.size()) page.next_cursor = "fixture-page-2";
    return page;
}
std::vector<PublicGame> GfnClient::FetchPublicGames() const
{
    std::lock_guard lock(fixture_mutex);
    ++calls.public_requests;
    auto public_games = catalog;
    for (auto& game : public_games) game.is_in_library = false;
    return public_games;
}
std::vector<LoginProvider> GfnClient::FetchLoginProviders() const { return {}; }
std::vector<StreamRegion> GfnClient::FetchStreamRegions(AuthSession&) const
{ std::lock_guard lock(fixture_mutex); ++calls.region_requests; return {{"Fixture region", "https://region.fixture.invalid/", -1}}; }
std::vector<StreamRegion> GfnClient::MeasureStreamRegionLatencies(std::vector<StreamRegion> regions) const
{
    std::lock_guard lock(fixture_mutex);
    ++calls.latency_measurements;
    for (auto& region : regions) region.ping_ms = 18;
    return regions;
}
std::string GfnClient::ProvisionCommunityProxy() const { return "https://fixture.invalid"; }
AuthSession GfnClient::LoginWithQrCode(const LoginProvider&, const std::function<void(const QrLoginChallenge&)>&,
                                    const std::function<bool()>&) const
{ throw std::runtime_error("Native fixture does not authorize an NVIDIA account"); }
AuthSession GfnClient::EnsureFreshSession(const AuthSession& session) const { return session; }
AuthSession GfnClient::EnsureFreshSavedSession(const AuthSession& session) const { return session; }
AuthSession GfnClient::RecoverSavedSession(const AuthSession& session, bool) const { return session; }
bool GfnClient::LoadSavedSession(AuthSession&) const { return false; }
std::vector<AuthSession> GfnClient::LoadSavedSessions() const { return {}; }
void GfnClient::ClearSavedSession() const {}
void GfnClient::SaveSession(const AuthSession&) const {}
bool GfnClient::SetActiveSavedSession(const std::string&) const { return true; }
std::string GfnClient::LoadLauncherPreference(const std::string&, const std::string&) const
{ std::lock_guard lock(fixture_mutex); return saved_variant; }
void GfnClient::SaveLauncherPreference(const std::string&, const std::string&, const std::string& variant) const
{ std::lock_guard lock(fixture_mutex); saved_variant = variant; calls.saved_variants.push_back(variant); }
SessionInfo GfnClient::StartSession(AuthSession& auth, const std::string& id, const StreamSettings& request_settings,
                                  const std::string& store, const std::string& title) const
{
    std::unique_lock lock(fixture_mutex);
    calls.launches.push_back({auth.user.user_id, id, title, store});
    calls.start_settings.push_back(request_settings);
    if (session_mode == ui_fixture::SessionMode::BlockStart &&
        !start_gate.wait_for(lock, std::chrono::seconds(8), [] { return start_released; }))
        throw std::runtime_error("Fixture start gate timed out");
    if (session_mode == ui_fixture::SessionMode::Failure)
        throw std::runtime_error("Fixture cloud request failed. " + std::string(256, 'x'));
    SessionInfo info;
    info.session_id = "fixture-session-" + std::to_string(calls.launches.size());
    info.status = session_mode == ui_fixture::SessionMode::Ready || session_mode == ui_fixture::SessionMode::BlockStart ? 2 :
        session_mode == ui_fixture::SessionMode::Confirmation ? 6 : session_mode == ui_fixture::SessionMode::Patching ? 1 : 0;
    info.queue_position = session_mode == ui_fixture::SessionMode::Unknown || info.status == 2 ? -1 : 123;
    info.app_patching = session_mode == ui_fixture::SessionMode::Patching;
    info.signaling_url = "fixture://signaling";
    return info;
}
SessionInfo GfnClient::PollSession(AuthSession&, const std::string& id) const
{ std::lock_guard lock(fixture_mutex); ++calls.polls; SessionInfo info; info.session_id = id; info.status = 0; info.queue_position = 7; return info; }
void GfnClient::StopSession(AuthSession&, const std::string& id) const
{ std::lock_guard lock(fixture_mutex); calls.stopped_sessions.push_back(id); }
void GfnClient::CleanupStaleCloudSession(AuthSession&) const {}
void PresentCloudStream(const SessionInfo& info, const GfnClient&, const AuthSession&, const std::string&, const StreamSettings& settings)
{ std::lock_guard lock(fixture_mutex); calls.handed_off_sessions.push_back(info.session_id); calls.handoff_settings.push_back(settings); }
std::string CurrentUtcIsoTimestamp() { return "2026-10-02T12:00:00Z"; }
bool RecordGamePlayed(const std::string& id, const std::string&, const std::string&)
{ std::lock_guard lock(fixture_mutex); calls.played_ids.push_back(id); return true; }
bool IsNevernessToEverness(const std::string& title) { return title.find("Neverness") != std::string::npos; }
bool NteCredentials::valid() const { return false; }
NteCredentials LoadNteCredentials() { return {}; }
bool SaveNteCredentials(const NteCredentials&) { return false; }
bool ClearNteCredentials() { return true; }
std::string NteCredentialsPath() { return {}; }
NetworkConnectionInfo NetworkUtils::GetConnectionInfo()
{ return {true, NetworkConnectionType::Wifi, network::WifiBand::Ghz5, 3}; }
namespace shortcut
{
const std::string& ExecutablePath() { static const std::string path = "fixture://SwitchNOW.nro"; return path; }
CreateResult CreateGameShortcut(LaunchRequest request)
{
    std::lock_guard lock(fixture_mutex);
    calls.shortcuts.push_back(request);
    return {true, false, request.title, "fixture://shortcut.nro", "fixture://manifest", {}};
}
bool StartForwarderInstaller(const std::string&, const std::string&, std::string&) { return true; }
}
}
