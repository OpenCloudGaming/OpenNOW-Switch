#pragma once

#include <borealis.hpp>

namespace opennow
{

class StatusTab : public brls::Box
{
  public:
    StatusTab();

  private:
    bool OpenArchitectureDialog(brls::View* view);
};

} // namespace opennow
