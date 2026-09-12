#pragma once

#include "gfn_client.hpp"
#include "models.hpp"

#include <borealis.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace opennow
{

class ProvidersTab : public brls::Box
{
  public:
    explicit ProvidersTab(std::function<void()> on_success = {});
    ~ProvidersTab() override;

    void willAppear(bool resetState) override;

  private:
    void ReloadProviders();
    void RebuildList();
    bool OpenProviderDialog(brls::View* view, size_t index);

    GfnClient client_;
    std::vector<LoginProvider> providers_;
    brls::Button* refresh_button_          = nullptr;
    brls::Label* status_label_             = nullptr;
    brls::ScrollingFrame* scrolling_frame_ = nullptr;
    brls::Box* list_container_             = nullptr;
    std::function<void()> on_success_;
    bool loading_                          = false;
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
};

} // namespace opennow
