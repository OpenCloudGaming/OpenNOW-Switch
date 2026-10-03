#pragma once

#include "stream_settings.hpp"
#include <borealis.hpp>
#include <array>
#include <functional>
#include <string>

namespace opennow
{

namespace ui { class ActionRow; }

struct QueueDisplayState
{
    std::string game_title;
    std::string status;
    std::string detail;
    int position = -1;
    int stage = 0;
    bool failed = false;
};

class QueueView final : public brls::Box
{
  public:
    QueueView(const QueueDisplayState& state, const StreamSettings& settings,
              std::function<void()> minimize, std::function<void()> cancel);
    void Update(const QueueDisplayState& state);

  private:
    brls::Label* game_;
    brls::Label* status_;
    brls::Label* position_;
    brls::Label* position_heading_;
    brls::Box* counter_;
    brls::Label* detail_;
    brls::Label* phase_;
    ui::ActionRow* minimize_;
    ui::ActionRow* cancel_;
    std::array<brls::Label*, 3> steps_ {};
};

}
