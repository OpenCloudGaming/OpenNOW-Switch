#pragma once

#include "stream_settings.hpp"

#include <borealis.hpp>

#include <functional>
#include <string>

namespace opennow::ui
{

enum class FontRole { Body, Medium, Heading, Display, Mono };
enum class ActionTone { Neutral, Primary, Destructive, Flat };

inline NVGcolor Ground() { return nvgRGB(11, 12, 14); }
inline NVGcolor Raised() { return nvgRGB(22, 24, 28); }
inline NVGcolor Text() { return nvgRGB(242, 244, 240); }
inline NVGcolor Muted() { return nvgRGB(163, 159, 150); }
inline NVGcolor Rule() { return nvgRGB(42, 45, 51); }
inline NVGcolor Green() { return nvgRGB(118, 232, 58); }
inline NVGcolor Danger() { return nvgRGB(255, 106, 85); }

void InitializeThemeAndFonts();
int Font(FontRole role);

class StyledLabel final : public brls::Label
{
  public:
    explicit StyledLabel(FontRole role = FontRole::Body);
};

brls::Label* MakeLabel(const std::string& text, float size = 18,
                      NVGcolor color = Text(), FontRole role = FontRole::Body);

class ActionRow : public brls::Box
{
  public:
    ActionRow(std::string title, std::string value,
              std::function<bool(brls::View*)> activate,
              ActionTone tone = ActionTone::Neutral);
    void SetTitle(const std::string& title);
    void SetValue(const std::string& value);
    void SetFontSize(float size);
    void SetTone(ActionTone tone);
    void SetFocusHandler(std::function<void()> handler);
    void onFocusGained() override;
    void onFocusLost() override;

  private:
    void UpdateChrome(bool focused);
    brls::Label* title_;
    brls::Label* value_;
    ActionTone tone_;
    std::function<void()> focus_handler_;
};

std::string StreamSummary(const StreamSettings& settings);
std::string ConfiguredLocation(const std::string& region);

class NextStreamSummaryView final : public brls::Box
{
  public:
    explicit NextStreamSummaryView(const StreamSettings& settings);
    void Update(const StreamSettings& settings);

  private:
    brls::Label* heading_;
    brls::Label* summary_;
    brls::Label* detail_;
};

}
