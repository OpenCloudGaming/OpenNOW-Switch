#include "queue_view.hpp"
#include "localization.hpp"
#include "ui_theme.hpp"

#include <algorithm>
#include <utility>

namespace opennow
{
namespace
{

class QueuePositionLabel final : public brls::Label
{
  public:
    QueuePositionLabel() { font = ui::Font(ui::FontRole::Display); }
    void setText(const std::string& value) override
    {
        value_ = value;
        brls::Label::setText(value);
    }
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style, brls::FrameContext*) override
    {
        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, width, height);
        nvgFontFaceId(vg, ui::Font(ui::FontRole::Display));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        const bool known = !value_.empty() && value_.front() >= '0' && value_.front() <= '9';
        const size_t count = known ? value_.size() : 1;
        const float card_width = known ? std::min(150.0f, (width - 12.0f * static_cast<float>(count - 1)) / static_cast<float>(count)) : std::min(340.0f, width);
        for (size_t index = 0; index < count; ++index)
        {
            const float card_x = x + static_cast<float>(index) * (card_width + 12);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, card_x, y, card_width, height, 12);
            nvgFillColor(vg, ui::Raised());
            nvgFill(vg);
            nvgStrokeWidth(vg, 1);
            nvgStrokeColor(vg, ui::Rule());
            nvgStroke(vg);
            nvgBeginPath(vg);
            nvgRect(vg, card_x, y + height * 0.5f, card_width, 1);
            nvgFillColor(vg, ui::Rule());
            nvgFill(vg);
            nvgFontSize(vg, known ? std::min(112.0f, card_width) : 58.0f);
            if (!known)
            {
                const float text_width = nvgTextBounds(vg, 0, 0, value_.c_str(), nullptr, nullptr);
                if (text_width > card_width - 24)
                    nvgFontSize(vg, 58.0f * (card_width - 24) / text_width);
            }
            nvgFillColor(vg, ui::Text());
            const char digit[] = {value_[index], '\0'};
            nvgText(vg, card_x + card_width * 0.5f, y + height * 0.5f,
                    known ? digit : value_.c_str(), nullptr);
        }
        nvgRestore(vg);
    }

  private:
    std::string value_;
};

}

QueueView::QueueView(const QueueDisplayState& state, const StreamSettings& settings,
                     std::function<void()> minimize, std::function<void()> cancel)
    : brls::Box(brls::Axis::COLUMN)
{
    setId("queue/root");
    setBackgroundColor(ui::Ground());
    auto* header = new brls::Box(brls::Axis::ROW);
    header->setHeight(76);
    header->setShrink(0);
    header->setPadding(0, 40, 0, 40);
    header->setAlignItems(brls::AlignItems::CENTER);
    header->setLineBottom(1);
    header->setLineColor(ui::Rule());
    auto* logo = new brls::Image();
    logo->setWidth(62);
    logo->setHeight(38);
    logo->setScalingType(brls::ImageScalingType::FIT);
    logo->setImageFromRes("img/opennow-logo-mark.png");
    logo->setMarginRight(10);
    header->addView(logo);
    auto* brand = ui::MakeLabel("OpenNOW", 22, ui::Text(), ui::FontRole::Heading);
    brand->setGrow(1);
    header->addView(brand);
    phase_ = ui::MakeLabel(state.status, 16, ui::Muted());
    phase_->setId("queue/phase");
    phase_->setMaxWidth(600);
    phase_->setSingleLine(true);
    header->addView(phase_);
    addView(header);

    auto* content = new brls::Box(brls::Axis::ROW);
    content->setGrow(1);
    content->setPadding(34, 40, 26, 40);
    auto* left = new brls::Box(brls::Axis::COLUMN);
    left->setGrow(1);
    left->setShrink(1);
    left->setMarginRight(64);
    left->addView(ui::MakeLabel(Tr("Position in queue"), 15, ui::Green(), ui::FontRole::Medium));
    game_ = ui::MakeLabel(state.game_title, 29, ui::Text(), ui::FontRole::Heading);
    game_->setId("queue/game");
    game_->setSingleLine(true);
    game_->setMarginTop(6);
    game_->setMarginBottom(20);
    left->addView(game_);
    auto* counter = new brls::Box(brls::Axis::COLUMN);
    counter->setHeight(170);
    counter->setWidthPercentage(100);
    counter->setJustifyContent(brls::JustifyContent::CENTER);
    position_ = new QueuePositionLabel();
    position_->setWidthPercentage(100);
    position_->setHeight(170);
    position_->setId("queue/position");
    position_->setSingleLine(true);
    counter->addView(position_);
    left->addView(counter);
    status_ = ui::MakeLabel("", 25, ui::Text(), ui::FontRole::Heading);
    status_->setId("queue/status");
    status_->setSingleLine(true);
    status_->setMarginTop(20);
    left->addView(status_);
    detail_ = ui::MakeLabel("", 18, ui::Muted());
    detail_->setId("queue/detail");
    auto* detail_scroll = new brls::ScrollingFrame();
    detail_scroll->setId("queue/detail-scroll");
    detail_scroll->setGrow(1);
    detail_scroll->setMarginTop(6);
    detail_scroll->setContentView(detail_);
    left->addView(detail_scroll);
    content->addView(left);

    auto* right = new brls::Box(brls::Axis::COLUMN);
    right->setWidth(480);
    right->setShrink(0);
    auto* steps = new brls::Box(brls::Axis::ROW);
    steps->setHeight(70);
    steps->setMarginTop(8);
    const char* names[] = {"Queue", "Setup", "Ready"};
    for (size_t index = 0; index < steps_.size(); ++index)
    {
        auto* column = new brls::Box(brls::Axis::COLUMN);
        column->setGrow(1);
        column->setMarginRight(index == 2 ? 0 : 8);
        column->setLineTop(5);
        column->setLineColor(ui::Rule());
        steps_[index] = ui::MakeLabel(Tr(names[index]), 16, ui::Muted());
        steps_[index]->setId("queue/step/" + std::to_string(index));
        steps_[index]->setMarginTop(10);
        column->addView(steps_[index]);
        steps->addView(column);
    }
    right->addView(steps);
    auto add_summary = [right](const std::string& id, const std::string& title, const std::string& value) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setHeight(46);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setLineBottom(1);
        row->setLineColor(ui::Rule());
        auto* label = ui::MakeLabel(Tr(title), 16, ui::Muted());
        label->setGrow(1);
        row->addView(label);
        auto* detail = ui::MakeLabel(value, 17, ui::Text(), ui::FontRole::Medium);
        detail->setId(id);
        detail->setMaxWidthPercentage(76);
        detail->setSingleLine(true);
        row->addView(detail);
        right->addView(row);
    };
    add_summary("queue/region", "Location", ui::ConfiguredLocation(settings.region));
    add_summary("queue/stream-summary", "Stream", ui::StreamSummary(settings));
    add_summary("queue/reminder", "Reminder", Tr("At position") + " " + std::to_string(settings.queue_notify_threshold));
    auto* summary = new ui::NextStreamSummaryView(settings);
    summary->setId("queue/next-stream");
    summary->setMarginTop(16);
    right->addView(summary);
    auto* minimize_row = new ui::ActionRow(Tr("Minimize and browse"), Tr("Queue keeps running"),
        [minimize](brls::View*) { minimize(); return true; });
    minimize_row->setId("queue/minimize");
    minimize_row->setMarginTop(22);
    right->addView(minimize_row);
    minimize_ = minimize_row;
    auto* cancel_row = new ui::ActionRow(Tr("Cancel session"), "",
        [cancel](brls::View*) { cancel(); return true; }, ui::ActionTone::Destructive);
    cancel_row->setId("queue/cancel");
    cancel_row->setMarginTop(12);
    right->addView(cancel_row);
    cancel_ = cancel_row;
    content->addView(right);
    addView(content);
    auto* footer = new brls::Box(brls::Axis::ROW);
    footer->setHeight(60);
    footer->setShrink(0);
    footer->setPadding(0, 40, 0, 40);
    footer->setAlignItems(brls::AlignItems::CENTER);
    footer->setLineTop(1);
    footer->setLineColor(ui::Rule());
    auto* select = ui::MakeLabel(Tr("A  Select    Up / Down  Choose"), 16, ui::Muted());
    select->setGrow(1);
    footer->addView(select);
    footer->addView(ui::MakeLabel(Tr("Minus + Plus  Reopen queue after minimizing"), 16, ui::Muted()));
    addView(footer);
    registerAction(Tr("Minimize"), brls::BUTTON_B,
        [minimize](brls::View*) { minimize(); return true; }, true);
    setLastFocusedView(minimize_row);
    Update(state);
}

void QueueView::Update(const QueueDisplayState& state)
{
    game_->setText(state.game_title);
    phase_->setText(Tr(state.status));
    status_->setText(Tr(state.status));
    status_->setTextColor(state.failed ? ui::Danger() : ui::Text());
    detail_->setText(Tr(state.detail));
    minimize_->SetTitle(Tr(state.failed ? "Back to game" : "Minimize and browse"));
    minimize_->SetValue(state.failed ? "" : Tr("Queue keeps running"));
    cancel_->SetTitle(Tr(state.failed ? "Close" : "Cancel session"));
    const std::string position = state.position >= 0
        ? (state.position < 10 ? "0" : "") + std::to_string(state.position) : Tr("Unknown");
    position_->setText(position);
    position_->setFontSize(state.position < 0 ? 62 : std::min(112.0f, 480.0f / static_cast<float>(position.size())));
    const int step = state.stage < 2 ? 0 : state.stage == 2 ? 1 : 2;
    for (size_t index = 0; index < steps_.size(); ++index)
    {
        const auto color = !state.failed && static_cast<int>(index) <= step ? ui::Green() : ui::Muted();
        steps_[index]->setTextColor(color);
        steps_[index]->getParent()->setLineColor(!state.failed && static_cast<int>(index) <= step ? ui::Green() : ui::Rule());
    }
}

}
