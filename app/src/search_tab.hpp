#pragma once

#include "gfn_client.hpp"
#include "models.hpp"

#include <borealis.hpp>

#include <vector>

namespace opennow
{

class SearchTab : public brls::Box
{
  public:
    SearchTab();

    void willAppear(bool resetState) override;

  private:
    void EnsureSessionLoaded();
    void OpenSearchIme();
    void RefreshData();
    void RebuildResults();
    void LoadMoreResults();
    bool OpenLibraryResult(size_t index);
    bool OpenCatalogResult(size_t index);

    GfnClient client_;
    std::vector<GameInfo> library_games_;
    std::vector<PublicGame> catalog_games_;
    brls::Label* status_label_ = nullptr;
    brls::Button* search_button_ = nullptr;
    brls::Button* refresh_button_ = nullptr;
    brls::Button* load_more_button_ = nullptr;
    brls::ScrollingFrame* scrolling_frame_ = nullptr;
    brls::Box* results_container_ = nullptr;
    std::string search_query_;
    bool loading_ = false;
    size_t visible_limit_ = 30;
    size_t result_count_ = 0;
};

} // namespace opennow
