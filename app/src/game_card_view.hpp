#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>

namespace opennow
{

struct GameCardDisplay
{
    std::string title;
    std::string subtitle;
    std::string badge;
    std::string image_url;
};

class GameCardView : public brls::Box
{
  public:
    using ClickHandler = std::function<void()>;

    GameCardView(GameCardDisplay display, ClickHandler click_handler);

    void onFocusGained() override;
    void onFocusLost() override;

  private:
    void UpdateChrome(bool focused);
    void LoadImage();

    GameCardDisplay display_;
    ClickHandler click_handler_;
    brls::Image* image_         = nullptr;
    brls::Label* title_label_   = nullptr;
    brls::Label* subtitle_label_ = nullptr;
};

} // namespace opennow
