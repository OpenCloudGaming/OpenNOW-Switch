#pragma once

#include <borealis.hpp>
#include "top_bar_frame.hpp"

#include <atomic>
#include <chrono>
#include <memory>

namespace opennow
{

class MainTabsView : public TopBarFrame
{
  public:
    MainTabsView();
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

  private:
    void MaybeRefreshAuthentication();

    std::chrono::steady_clock::time_point last_auth_check_ {};
    std::shared_ptr<std::atomic<bool>> auth_refresh_running_ =
        std::make_shared<std::atomic<bool>>(false);
};

} // namespace opennow
