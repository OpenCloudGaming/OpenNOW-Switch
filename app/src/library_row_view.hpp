#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>

namespace opennow
{

struct LibraryRowDisplay
{
    std::string identity;
    std::string title;
    std::string store;
    std::string membership;
    std::string time;
    std::string image_url;
};

class LibraryRowView final : public brls::Box
{
  public:
    LibraryRowView(LibraryRowDisplay display, std::function<void()> selected,
                   std::function<void()> opened);
    void onFocusGained() override;
    void onFocusLost() override;

  private:
    void UpdateChrome(bool focused);
    brls::Label* time_ = nullptr;
    brls::Label* prompt_ = nullptr;
    std::function<void()> selected_;
};

}
