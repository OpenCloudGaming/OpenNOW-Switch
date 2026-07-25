#pragma once

#include "gfn_client.hpp"
#include "models.hpp"

#include <borealis.hpp>

#include <functional>
#include <vector>

namespace opennow
{

class ProvidersTab : public brls::Box
{
  public:
    explicit ProvidersTab(std::function<void()> on_success = {});

    void willAppear(bool resetState) override;

  private:
    void ReloadProviders();
    void RebuildList();
    bool OpenProviderDialog(brls::View* view, size_t index);

    GfnClient client_;
    std::vector<LoginProvider> providers_;
    brls::Label* status_label_             = nullptr;
    brls::ScrollingFrame* scrolling_frame_ = nullptr;
    brls::Box* list_container_             = nullptr;
    std::function<void()> on_success_;
    bool loading_                          = false;
};

} // namespace opennow
