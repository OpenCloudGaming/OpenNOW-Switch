#pragma once

#include <borealis.hpp>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace opennow
{

class TopBarFrame : public brls::Box
{
  public:
    using TabViewCreator = std::function<brls::View*()>;

    TopBarFrame();
    ~TopBarFrame() override;

    void addTab(const std::string& label, TabViewCreator creator);
    void focusTab(int position);

    void onFocusGained() override;
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;
    
  private:
    void SelectTab(int index);
    void UpdateStatusBar(bool force = false);

    brls::Box* header_container_;
    brls::Box* tabs_container_;
    brls::Box* content_container_;
    brls::Box* account_container_;
    brls::Box* subscription_container_;
    brls::Image* avatar_image_;
    brls::Label* time_remaining_label_;
    brls::Label* storage_remaining_label_;
    brls::Label* account_name_label_;
    brls::Label* account_detail_label_;
    std::string displayed_avatar_url_;
    std::string displayed_status_;
    std::chrono::steady_clock::time_point last_status_update_ {};

    struct TabInfo {
        std::string label;
        TabViewCreator creator;
        brls::Label* header_label;
        brls::Rectangle* underline;
        brls::Box* tab_box;
        brls::View* content = nullptr;
    };

    std::vector<TabInfo> tabs_;
    int active_tab_index_ = -1;
    brls::View* active_content_ = nullptr;
};

} // namespace opennow
