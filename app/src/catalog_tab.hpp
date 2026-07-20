#pragma once

#include "gfn_client.hpp"
#include "models.hpp"

#include <borealis.hpp>

#include <atomic>
#include <memory>
#include <vector>

namespace opennow
{

class CatalogTab : public brls::Box
{
  public:
    CatalogTab();
    ~CatalogTab() override;

    void willAppear(bool resetState) override;

  private:
    void ReloadCatalog(const std::string& server_query = {});
    void BeginSearch();
    void RebuildList();
    void LoadMoreOrRefresh();
    void EnsurePagingButton();
    void DetachPagingButton();
    void AttachPagingButton(bool has_more, size_t remaining);
    void HandlePagingButton();
    void HandlePreviousPage();
    void CycleStoreFilter();
    void CycleSortMode();
    bool OpenGameDialog(brls::View* view, size_t index);

    GfnClient client_;
    std::vector<PublicGame> games_;
    std::vector<PublicGame> base_games_;
    std::vector<std::vector<brls::View*>> card_rows_;
    std::vector<brls::View*> toolbar_buttons_;
    brls::Label* status_label_                 = nullptr;
    brls::Button* search_button_               = nullptr;
    brls::Button* filter_button_               = nullptr;
    brls::Button* sort_button_                 = nullptr;
    brls::Button* more_button_                 = nullptr;
    brls::Button* previous_page_button_        = nullptr;
    brls::Button* load_more_button_            = nullptr;
    brls::Box* paging_container_               = nullptr;
    brls::ScrollingFrame* scrolling_frame_     = nullptr;
    brls::Box* list_container_                 = nullptr;
    brls::View* first_card_                    = nullptr;
    bool loading_                              = false;
    bool rebuilding_                           = false;
    bool load_more_pending_                    = false;
    bool paging_button_attached_               = false;
    std::shared_ptr<std::atomic_bool> alive_   = std::make_shared<std::atomic_bool>(true);
    std::string search_query_                  = "";
    size_t filtered_count_                     = 0;
    size_t page_index_                         = 0;
    size_t store_filter_index_                 = 0;
    size_t sort_mode_index_                    = 0;
};

} // namespace opennow
