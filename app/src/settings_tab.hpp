#pragma once

#include "gfn_client.hpp"

#include <borealis.hpp>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "stream_settings.hpp"

namespace opennow
{

class SettingsTab : public brls::Box
{
  public:
    SettingsTab();

    void willAppear(bool resetState) override;

  private:
    enum class Category
    {
        Account,
        Stream,
        Preferences,
        App,
    };

    void EnsureSessionLoaded();
    void RefreshSummary();
    void SelectCategory(Category category);
    void RebuildCategory();
    void BuildAccountPage();
    void BuildStreamPage();
    void BuildPreferencesPage();
    void BuildAppPage();
    void UpdateCategoryChrome();
    void UpdateOptionValues();
    void MarkDirty();
    bool SaveChanges(brls::View* view);
    bool RevertChanges(brls::View* view);
    bool CycleResolution(brls::View* view);
    bool CycleFrameRate(brls::View* view);
    bool CycleBitrate(brls::View* view);
    bool CycleVideoBackend(brls::View* view);
    bool CycleImageQuality(brls::View* view);
    bool ToggleStatsOverlay(brls::View* view);
    bool ChooseServerLocation(brls::View* view);
    bool RefreshServerLocations(brls::View* view);
    void BeginServerLocationLoad(bool open_when_ready, bool notify_result);
    std::string ServerLocationValue() const;

    brls::Box* MakeSection(const std::string& title, const std::string& subtitle = {});
    brls::Box* MakeOptionRow(
        const std::string& title,
        const std::string& description,
        std::function<std::string()> value,
        std::function<bool(brls::View*)> action);
    brls::Box* MakeActionRow(
        const std::string& title,
        const std::string& description,
        const std::string& button_text,
        std::function<bool(brls::View*)> action,
        bool destructive = false);
    void AddInfoLine(brls::Box* parent, const std::string& label, const std::string& value);

    bool ClearSavedLogin(brls::View* view);
    bool SwitchSavedAccount(brls::View* view);
    bool BeginLogin(brls::View* view);
    bool ClearCoverCache(brls::View* view);
    bool ToggleCommunityProxy(brls::View* view);
    bool ChooseGameLanguage(brls::View* view);
    bool TogglePersistGameSettings(brls::View* view);
    bool ToggleControllerLayout(brls::View* view);
    bool ToggleAudio(brls::View* view);
    bool CycleAudioVolume(brls::View* view);
    bool ChooseInterfaceLanguage(brls::View* view);

    GfnClient client_;
    brls::Label* page_title_ = nullptr;
    brls::Label* page_subtitle_ = nullptr;
    brls::Label* save_status_ = nullptr;
    brls::ScrollingFrame* scrolling_frame_ = nullptr;
    brls::Box* content_container_ = nullptr;
    struct CategoryNavItem
    {
        brls::Box* row = nullptr;
        brls::Rectangle* marker = nullptr;
        brls::Label* label = nullptr;
    };

    std::vector<CategoryNavItem> category_nav_items_;
    std::vector<std::pair<brls::Button*, std::function<std::string()>>> option_values_;
    Category category_ = Category::Account;
    StreamSettings saved_settings_;
    StreamSettings draft_settings_;
    bool settings_loaded_ = false;
    bool dirty_ = false;
    bool community_proxy_provisioning_ = false;
    std::vector<StreamRegion> server_locations_;
    bool server_locations_loaded_ = false;
    bool server_locations_loading_ = false;
    bool open_server_location_when_ready_ = false;
    std::string server_locations_error_;
};

} // namespace opennow
