#pragma once

#include "gfn_client.hpp"
#include "models.hpp"

#include <borealis.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace opennow
{

class LibraryTab : public brls::Box
{
  public:
    LibraryTab();
    ~LibraryTab() override;

    void willAppear(bool resetState) override;

  private:
    void EnsureSessionLoaded();
    void UpdateSessionUi();
    void BeginLogin();
    void Logout();
    void ReloadLibrary(bool background = false);
    void BeginSearch();
    void RebuildGrid();
    void LoadMore();
    void LoadMoreOrRefresh();
    void CycleStoreFilter();
    void CycleSortMode();
    bool OpenGameDialog(size_t index);

    GfnClient client_;
    std::vector<GameInfo> games_;
    std::vector<std::vector<brls::View*>> card_rows_;
    std::vector<brls::View*> account_buttons_;
    std::vector<brls::View*> toolbar_buttons_;
    brls::Label* account_label_         = nullptr;
    brls::Label* status_label_          = nullptr;
    brls::Button* search_button_        = nullptr;
    brls::Button* filter_button_        = nullptr;
    brls::Button* sort_button_          = nullptr;
    brls::Button* more_button_          = nullptr;
    brls::Button* sign_in_button_       = nullptr;
    brls::Button* refresh_button_       = nullptr;
    brls::Button* logout_button_        = nullptr;
    brls::Button* load_more_button_     = nullptr;
    brls::ScrollingFrame* scrolling_frame_ = nullptr;
    brls::Box* grid_container_          = nullptr;
    brls::View* first_card_             = nullptr;
    bool loading_                       = false;
    bool rebuilding_                    = false;
    bool focus_new_cards_after_load_    = false;
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
    std::string search_query_           = "";
    size_t visible_limit_               = 15;
    size_t current_grid_size_           = 0;
    size_t filtered_count_              = 0;
    size_t store_filter_index_          = 0;
    size_t sort_mode_index_             = 0;
    std::chrono::steady_clock::time_point last_library_sync_ {};
};

} // namespace opennow
