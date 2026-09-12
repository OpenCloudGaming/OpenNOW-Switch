#include "settings_tab.hpp"

#include "app_state.hpp"
#include "cover_image_cache.hpp"
#include "home_shortcut.hpp"
#include "localization.hpp"
#include "server_location_policy.hpp"
#include "stream_settings_policy.hpp"
#include "ui_helpers.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace opennow
{

bool SettingsTab::ClearCoverCache(brls::View* view)
{
    (void)view;
    if (cover_cache_action_)
        brls::Application::notify("Cover cache work is already running");
    else
        BeginCoverCacheWork(CacheAction::Clear);
    return true;
}

void SettingsTab::UpdateCoverCacheValues()
{
    if (!cover_cache_files_ || !cover_cache_bytes_)
        return;

    if (cover_cache_action_)
    {
        cover_cache_files_->setText(Tr(
            *cover_cache_action_ == CacheAction::Clear ? "Clearing..." : "Checking..."));
        cover_cache_bytes_->setText("--");
        return;
    }

    cover_cache_files_->setText(
        cover_cache_stats_ ? std::to_string(cover_cache_stats_->files) : "--");
    cover_cache_bytes_->setText(
        cover_cache_stats_ ? FormatBytes(cover_cache_stats_->bytes) : "--");
}

void SettingsTab::BeginCoverCacheWork(CacheAction action)
{
    if (cover_cache_action_)
        return;

    cover_cache_action_ = action;
    UpdateCoverCacheValues();
    const auto alive = alive_;
    brls::async([this, alive, action] {
        if (!alive->load())
            return;
        try
        {
            const size_t removed = action == CacheAction::Clear ? ClearCoverImageCache() : 0;
            const auto stats = InspectCoverImageCache();
            brls::sync([this, alive, action, removed, stats] {
                if (!alive->load())
                    return;
                cover_cache_stats_ = stats;
                cover_cache_action_.reset();
                UpdateCoverCacheValues();
                if (action == CacheAction::Clear)
                    brls::Application::notify(
                        "Removed " + std::to_string(removed) + " cached cover files");
            });
        }
        catch (const std::exception& ex)
        {
            const std::string error = ex.what();
            brls::sync([this, alive, error] {
                if (!alive->load())
                    return;
                cover_cache_action_.reset();
                UpdateCoverCacheValues();
                ShowError("Cover Cache Failed", error);
            });
        }
    }, false);
}

bool SettingsTab::CycleResolution(brls::View* view)
{
    (void)view;
    settings::CycleResolution(draft_settings_);
    MarkDirty();
    return true;
}

bool SettingsTab::CycleFrameRate(brls::View* view)
{
    (void)view;
    settings::CycleFrameRate(draft_settings_);
    MarkDirty();
    return true;
}

bool SettingsTab::CycleBitrate(brls::View* view)
{
    (void)view;
    settings::CycleBitrate(draft_settings_);
    MarkDirty();
    return true;
}

bool SettingsTab::CycleVideoBackend(brls::View* view)
{
    (void)view;
    settings::CycleVideoBackend(draft_settings_);
    MarkDirty();
    return true;
}

std::string SettingsTab::ServerLocationValue() const
{
    if (server_locations_loading_)
        return "Testing locations...";

    const auto best = std::find_if(
        server_locations_.begin(), server_locations_.end(),
        [](const StreamRegion& region) { return region.ping_ms >= 0; });

    if (server_location::IsAutomatic(draft_settings_.region))
    {
        if (best == server_locations_.end())
            return "Auto (best)";
        return "Auto · " + best->name + " · " +
            std::to_string(best->ping_ms) + " ms";
    }

    const auto selected = std::find_if(
        server_locations_.begin(), server_locations_.end(),
        [this](const StreamRegion& region) {
            return region.url == draft_settings_.region;
        });
    if (selected == server_locations_.end())
        return "Selected server";

    std::string value = selected->name;
    if (selected->ping_ms >= 0)
        value += " · " + std::to_string(selected->ping_ms) + " ms";
    return value;
}

void SettingsTab::SyncServerLocationAccount()
{
    const auto generation = AppState::Instance().session_generation();
    if (server_locations_generation_ == generation)
        return;

    server_locations_generation_ = generation;
    server_locations_.clear();
    server_locations_loaded_ = false;
    server_locations_loading_ = false;
    open_server_location_when_ready_ = false;
    server_locations_error_.clear();
}

void SettingsTab::BeginServerLocationLoad(
    bool open_when_ready,
    bool notify_result)
{
    EnsureSessionLoaded();
    SyncServerLocationAccount();
    if (server_locations_loading_)
    {
        open_server_location_when_ready_ =
            open_server_location_when_ready_ || open_when_ready;
        if (notify_result)
            brls::Application::notify("Server location test is already running");
        return;
    }

    auto& state = AppState::Instance();
    if (!state.HasSession())
    {
        if (open_when_ready || notify_result)
        {
            ShowError(
                "Server Locations Unavailable",
                "Connect a GeForce NOW account before loading server locations.");
        }
        return;
    }

    server_locations_loading_ = true;
    server_locations_error_.clear();
    open_server_location_when_ready_ = open_when_ready;
    UpdateOptionValues();

    GfnClient client = client_;
    AuthSession session = *state.session();
    const auto generation = state.session_generation();
    const auto alive = alive_;
    brls::async(
        [this, alive, generation, client, session = std::move(session), notify_result]() mutable {
            if (!alive->load())
                return;
            try
            {
                std::vector<StreamRegion> regions =
                    client.FetchStreamRegions(session);
                regions = client.MeasureStreamRegionLatencies(std::move(regions));
                std::stable_sort(
                    regions.begin(), regions.end(),
                    [](const StreamRegion& left, const StreamRegion& right) {
                        const bool left_ok = left.ping_ms >= 0;
                        const bool right_ok = right.ping_ms >= 0;
                        if (left_ok != right_ok)
                            return left_ok;
                        if (left_ok && left.ping_ms != right.ping_ms)
                            return left.ping_ms < right.ping_ms;
                        return left.name < right.name;
                    });

                brls::sync(
                    [this, alive, generation, session = std::move(session),
                     regions = std::move(regions), notify_result]() mutable {
                        if (!alive->load())
                            return;
                        auto& current = AppState::Instance();
                        if (!current.IsCurrentSession(generation))
                            return;
                        current.SetSession(std::move(session));

                        server_locations_ = std::move(regions);
                        server_locations_loaded_ = true;
                        server_locations_loading_ = false;
                        if (server_locations_.empty())
                        {
                            server_locations_error_ =
                                "GeForce NOW did not return any server locations.";
                        }
                        UpdateOptionValues();

                        const bool should_open =
                            open_server_location_when_ready_ &&
                            category_ == Category::Stream;
                        open_server_location_when_ready_ = false;
                        if (should_open)
                        {
                            ChooseServerLocation(nullptr);
                        }
                        else if (notify_result)
                        {
                            brls::Application::notify(
                                server_locations_.empty()
                                    ? server_locations_error_
                                    : "Server location latency test complete");
                        }
                    });
            }
            catch (const std::exception& ex)
            {
                const std::string message = ex.what();
                brls::sync([this, alive, generation, message, notify_result] {
                    if (!alive->load() || !AppState::Instance().IsCurrentSession(generation))
                        return;
                    const bool should_open =
                        open_server_location_when_ready_ &&
                        category_ == Category::Stream;
                    server_locations_loading_ = false;
                    server_locations_loaded_ =
                        !server_locations_.empty() || should_open;
                    server_locations_error_ = message;
                    open_server_location_when_ready_ = false;
                    UpdateOptionValues();
                    if (should_open)
                    {
                        brls::Application::notify(
                            "Latency test failed; automatic routing is still available");
                        ChooseServerLocation(nullptr);
                    }
                    else if (notify_result)
                    {
                        ShowError("Server Location Test Failed", message);
                    }
                });
            }
        },
        false);
}

bool SettingsTab::ChooseServerLocation(brls::View* view)
{
    (void)view;
    SyncServerLocationAccount();
    if (server_locations_loading_)
    {
        open_server_location_when_ready_ = true;
        brls::Application::notify("Testing GeForce NOW server locations...");
        return true;
    }

    if (!server_locations_loaded_)
    {
        BeginServerLocationLoad(true, false);
        return true;
    }

    const auto best = std::find_if(
        server_locations_.begin(), server_locations_.end(),
        [](const StreamRegion& region) { return region.ping_ms >= 0; });

    std::vector<StreamRegion> options = server_locations_;
    if (!server_location::IsAutomatic(draft_settings_.region))
    {
        const bool selected_available = std::any_of(
            options.begin(), options.end(), [this](const StreamRegion& region) {
                return region.url == draft_settings_.region;
            });
        if (!selected_available)
        {
            options.push_back(
                {"Current server (not advertised)", draft_settings_.region, -1});
        }
    }

    std::vector<std::string> labels;
    labels.reserve(options.size() + 1);
    std::string automatic = "Auto (best)";
    if (best != server_locations_.end())
    {
        automatic += " — " + best->name + " · " +
            std::to_string(best->ping_ms) + " ms";
    }
    labels.push_back(std::move(automatic));

    int selected_index = 0;
    for (size_t index = 0; index < options.size(); ++index)
    {
        const StreamRegion& region = options[index];
        std::string label = region.name;
        if (region.ping_ms >= 0)
            label += " — " + std::to_string(region.ping_ms) + " ms";
        else
            label += " — Unavailable";
        if (best != server_locations_.end() && region.url == best->url)
            label += " · Best";
        labels.push_back(std::move(label));

        if (region.url == draft_settings_.region)
            selected_index = static_cast<int>(index + 1);
    }

    const auto alive = alive_;
    const auto generation = AppState::Instance().session_generation();
    auto* dropdown = new brls::Dropdown(
        "Server location", labels,
        [this, alive, generation, options = std::move(options)](int index) {
            if (!alive->load() || !AppState::Instance().IsCurrentSession(generation) || index < 0)
                return;
            draft_settings_.region =
                index == 0 || static_cast<size_t>(index) > options.size()
                ? "Auto"
                : options[static_cast<size_t>(index - 1)].url;
            MarkDirty();
            UpdateOptionValues();
        },
        selected_index);
    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}

bool SettingsTab::RefreshServerLocations(brls::View* view)
{
    (void)view;
    BeginServerLocationLoad(false, true);
    return true;
}

bool SettingsTab::ToggleCommunityProxy(brls::View* view)
{
    (void)view;
    if (community_proxy_provisioning_)
        return true;

    if (draft_settings_.community_proxy_enabled)
    {
        draft_settings_.community_proxy_enabled = false;
        MarkDirty();
        return true;
    }

    const auto alive = alive_;
    auto* dialog = new brls::Dialog(
        "The Zortos community proxy is optional, shared and may be rate-limited or "
        "unavailable. It only routes NVIDIA catalog and session requests; streaming "
        "traffic stays direct.");
    dialog->addButton("Enable proxy", [this, alive]() {
        if (!alive->load())
            return;
        const auto generation = ++proxy_request_generation_;
        community_proxy_provisioning_ = true;
        UpdateOptionValues();
        brls::Application::notify("Activating community proxy...");

        GfnClient client = client_;
        brls::async([this, alive, generation, client]() mutable {
            if (!alive->load())
                return;
            try
            {
                std::string proxy_url = client.ProvisionCommunityProxy();
                brls::sync([this, alive, generation, proxy_url = std::move(proxy_url)]() mutable {
                    if (!alive->load() || generation != proxy_request_generation_)
                        return;
                    draft_settings_.community_proxy_url = std::move(proxy_url);
                    draft_settings_.community_proxy_enabled = true;
                    community_proxy_provisioning_ = false;
                    MarkDirty();
                    UpdateOptionValues();
                    brls::Application::notify(
                        "Community proxy ready; press X to save");
                });
            }
            catch (const std::exception& ex)
            {
                const std::string message = ex.what();
                brls::sync([this, alive, generation, message] {
                    if (!alive->load() || generation != proxy_request_generation_)
                        return;
                    community_proxy_provisioning_ = false;
                    UpdateOptionValues();
                    ShowError("Community Proxy Failed", message);
                });
            }
        }, false);
    });
    dialog->addButton("Cancel", [] {});
    dialog->open();
    return true;
}

bool SettingsTab::CycleImageQuality(brls::View* view)
{
    (void)view;
    settings::CycleImageQuality(draft_settings_);
    MarkDirty();
    return true;
}

bool SettingsTab::ToggleStatsOverlay(brls::View* view)
{
    (void)view;
    draft_settings_.stats_overlay_enabled = !draft_settings_.stats_overlay_enabled;
    MarkDirty();
    return true;
}

bool SettingsTab::ChooseGameLanguage(brls::View* view)
{
    (void)view;
    const auto& options = GameLanguageOptions();
    std::vector<std::string> labels;
    labels.reserve(options.size());
    int selected = 0;
    for (size_t index = 0; index < options.size(); ++index)
    {
        labels.push_back(options[index].label);
        if (options[index].code == draft_settings_.game_language)
            selected = static_cast<int>(index);
    }

    auto* dropdown = new brls::Dropdown(
        "Game language", labels,
        [this, alive = alive_](int index) {
            if (!alive->load())
                return;
            const auto& languages = GameLanguageOptions();
            if (index < 0 || static_cast<size_t>(index) >= languages.size())
                return;
            draft_settings_.game_language = languages[static_cast<size_t>(index)].code;
            MarkDirty();
            UpdateOptionValues();
        },
        selected);
    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}

bool SettingsTab::ShowHomeScreenHelp(brls::View* view)
{
    (void)view;
    auto* dialog = new brls::Dialog(
        "OpenNOW will generate a Horizon forwarder and install it to SD "
        "storage. The HOME icon starts the existing OpenNOW NRO, so future "
        "OpenNOW updates do not require reinstalling the forwarder.\n\n"
        "Continue?");
    dialog->addButton("Install on HOME", [] {
        std::string error;
        if (!shortcut::StartForwarderInstaller(
                shortcut::ExecutablePath(), "OpenNOW", error))
            ShowError("HOME Install Failed", error);
    });
    dialog->addButton("Cancel", [] {});
    dialog->setCancelable(true);
    dialog->open();
    return true;
}

bool SettingsTab::ChooseInterfaceLanguage(brls::View* view)
{
    (void)view;
    const auto& options = InterfaceLanguageOptions();
    std::vector<std::string> labels;
    labels.reserve(options.size());
    int selected = 0;
    for (size_t index = 0; index < options.size(); ++index)
    {
        labels.push_back(options[index].label);
        if (options[index].code == draft_settings_.interface_language)
            selected = static_cast<int>(index);
    }

    auto* dropdown = new brls::Dropdown(
        Tr("Language"), labels,
        [this, alive = alive_](int index) {
            if (!alive->load())
                return;
            const auto& languages = InterfaceLanguageOptions();
            if (index < 0 || static_cast<size_t>(index) >= languages.size())
                return;
            draft_settings_.interface_language = languages[static_cast<size_t>(index)].code;
            MarkDirty();
            UpdateOptionValues();
        },
        selected);
    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}

bool SettingsTab::TogglePersistGameSettings(brls::View* view)
{
    (void)view;
    draft_settings_.persist_game_settings = !draft_settings_.persist_game_settings;
    MarkDirty();
    return true;
}

bool SettingsTab::ToggleControllerLayout(brls::View* view)
{
    (void)view;
    draft_settings_.controller_layout =
        draft_settings_.controller_layout == "Switch" ? "Xbox" : "Switch";
    MarkDirty();
    return true;
}

bool SettingsTab::ToggleAudio(brls::View* view)
{
    (void)view;
    draft_settings_.audio_enabled = !draft_settings_.audio_enabled;
    MarkDirty();
    return true;
}

bool SettingsTab::CycleAudioVolume(brls::View* view)
{
    (void)view;
    constexpr int gains[] = {800, 1000, 1200, 1400, 1600};
    int next_gain = gains[0];
    for (int gain : gains)
    {
        if (gain > draft_settings_.audio_volume)
        {
            next_gain = gain;
            break;
        }
    }
    draft_settings_.audio_volume = next_gain;
    MarkDirty();
    return true;
}

bool SettingsTab::CycleQueueNotifyThreshold(brls::View* view)
{
    (void)view;
    constexpr int thresholds[] = {5, 10, 20, 50};
    int next = thresholds[0];
    for (int v : thresholds) {
        if (v > draft_settings_.queue_notify_threshold) { next = v; break; }
    }
    draft_settings_.queue_notify_threshold = next;
    MarkDirty();
    return true;
}

} // namespace opennow
