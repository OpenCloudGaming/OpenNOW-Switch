#pragma once

#include "gfn_client.hpp"
#include "models.hpp"
#include "ui_theme.hpp"

#include <borealis.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace opennow
{

class CachedImage;

class LibraryTab : public brls::Box
{
  public:
    LibraryTab();
    ~LibraryTab() override;

    void willAppear(bool resetState) override;

  private:
    void EnsureSessionLoaded();
    void UpdateSessionUi();
    void ReloadLibrary(bool background = false);
    void BeginSearch();
    void RebuildList();
    void LoadMoreOrRefresh();
    void PreviousPage();
    void ChangePage(size_t page);
    void SelectGame(const std::string& identity);
    void UpdatePreview();
    void CycleStoreFilter();
    void CycleSortMode();
    bool OpenGameDialog(const std::string& identity);

    GfnClient client_;
    std::vector<GameInfo> games_;
    std::vector<brls::View*> rows_;
    std::vector<brls::View*> toolbar_buttons_;
    brls::Label* heading_               = nullptr;
    brls::Label* selection_label_       = nullptr;
    brls::Label* status_label_          = nullptr;
    ui::ActionRow* search_button_      = nullptr;
    ui::ActionRow* filter_button_      = nullptr;
    ui::ActionRow* sort_button_        = nullptr;
    ui::ActionRow* more_button_        = nullptr;
    ui::ActionRow* previous_button_    = nullptr;
    ui::ActionRow* next_button_        = nullptr;
    brls::ScrollingFrame* scrolling_frame_ = nullptr;
    brls::Box* list_container_          = nullptr;
    brls::Box* preview_container_       = nullptr;
    CachedImage* preview_image_        = nullptr;
    brls::Label* preview_title_         = nullptr;
    brls::Label* preview_store_         = nullptr;
    brls::Label* preview_membership_    = nullptr;
    brls::Label* preview_last_played_   = nullptr;
    ui::NextStreamSummaryView* stream_summary_ = nullptr;
    bool loading_                       = false;
    bool rebuilding_                    = false;
    bool page_pending_                 = false;
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
    std::string search_query_           = "";
    size_t page_index_                  = 0;
    size_t filtered_count_              = 0;
    size_t store_filter_index_          = 0;
    size_t sort_mode_index_             = 0;
    std::string selected_identity_;
    std::string preview_identity_;
    std::string preview_image_url_;
    std::uint64_t library_session_generation_ = 0;
    std::chrono::steady_clock::time_point last_library_sync_ {};
};

} // namespace opennow
