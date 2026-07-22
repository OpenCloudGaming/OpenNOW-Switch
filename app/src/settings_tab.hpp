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
        Game,
        Controls,
        Audio,
        Storage,
        Interface,
    };

    void EnsureSessionLoaded();
    void RefreshSummary();
    void SelectCategory(Category category);
    void RebuildCategory();
    void BuildAccountPage();
    void BuildStreamPage();
    void BuildGamePage();
    void BuildControlsPage();
    void BuildAudioPage();
    void BuildStoragePage();
    void BuildInterfacePage();
    void UpdateCategoryChrome();
    void UpdateOptionValues();
    void MarkDirty();
    bool SaveChanges(brls::View* view);
    bool RevertChanges(brls::View* view);
    bool CycleResolution(brls::View* view);
    bool CycleFrameRate(brls::View* view);
    bool CycleBitrate(brls::View* view);
    bool CycleImageQuality(brls::View* view);

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

    bool ShowSessionDialog(brls::View* view);
    bool TestTokenRefresh(brls::View* view);
    bool ClearSavedLogin(brls::View* view);
    bool SwitchSavedAccount(brls::View* view);
    bool ClearAllSavedLogins(brls::View* view);
    bool ShowCacheState(brls::View* view);
    bool ClearCoverCache(brls::View* view);
    bool CycleStreamPreset(brls::View* view);
    bool ResetStreamPreset(brls::View* view);
    bool CycleVideoBackend(brls::View* view);
    bool ChooseGameLanguage(brls::View* view);
    bool TogglePersistGameSettings(brls::View* view);
    bool ToggleControllerLayout(brls::View* view);
    bool ToggleAudio(brls::View* view);
    bool CycleAudioVolume(brls::View* view);
    bool CycleAudioBuffer(brls::View* view);
    bool ToggleDebugDiagnostics(brls::View* view);
    bool ShowDiagnostics(brls::View* view);
    bool ChooseInterfaceLanguage(brls::View* view);

    GfnClient client_;
    brls::Label* page_title_ = nullptr;
    brls::Label* page_subtitle_ = nullptr;
    brls::Label* save_status_ = nullptr;
    brls::ScrollingFrame* scrolling_frame_ = nullptr;
    brls::Box* content_container_ = nullptr;
    std::vector<brls::Button*> category_buttons_;
    std::vector<std::pair<brls::Button*, std::function<std::string()>>> option_values_;
    Category category_ = Category::Stream;
    StreamSettings saved_settings_;
    StreamSettings draft_settings_;
    bool settings_loaded_ = false;
    bool dirty_ = false;
};

} // namespace opennow
