#include "top_bar_frame.hpp"
#include "app_state.hpp"
#include "avatar_utils.hpp"
#include "cover_image_cache.hpp"
#include "localization.hpp"
#include "membership_tier_policy.hpp"
#include "membership_tier_style.hpp"
#include "subscription_display.hpp"
#include "ui_refresh_policy.hpp"
#include <borealis/core/application.hpp>
#include <borealis/core/theme.hpp>
#include <borealis/core/logger.hpp>
#include <algorithm>

namespace opennow
{
namespace
{

enum class SubscriptionIcon
{
    Timer,
    HardDrive,
};

class SubscriptionIconView final : public brls::View
{
  public:
    explicit SubscriptionIconView(SubscriptionIcon icon)
        : icon_(icon)
    {
        setWidth(17);
        setHeight(17);
        setShrink(0.0f);
        setMarginRight(6);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override
    {
        (void)style;
        (void)ctx;

        // OpenNOW Desktop uses Lucide's Timer and HardDrive icons. Keep the
        // same 24-unit geometry here so the native header matches it.
        const float scale = std::min(width, height) / 24.0f;
        const float left = x + (width - 24.0f * scale) * 0.5f;
        const float top = y + (height - 24.0f * scale) * 0.5f;
        const auto px = [left, scale](float value) { return left + value * scale; };
        const auto py = [top, scale](float value) { return top + value * scale; };

        nvgBeginPath(vg);
        if (icon_ == SubscriptionIcon::Timer)
        {
            nvgMoveTo(vg, px(10), py(2));
            nvgLineTo(vg, px(14), py(2));
            nvgMoveTo(vg, px(12), py(14));
            nvgLineTo(vg, px(15), py(11));
            nvgCircle(vg, px(12), py(14), 8.0f * scale);
        }
        else
        {
            nvgMoveTo(vg, px(2.212f), py(11.577f));
            nvgLineTo(vg, px(5.45f), py(5.11f));
            nvgBezierTo(vg, px(5.789f), py(4.432f), px(6.482f), py(4), px(7.24f), py(4));
            nvgLineTo(vg, px(16.76f), py(4));
            nvgBezierTo(vg, px(17.518f), py(4), px(18.211f), py(4.432f), px(18.55f), py(5.11f));
            nvgLineTo(vg, px(21.788f), py(11.577f));
            nvgBezierTo(vg, px(21.927f), py(11.855f), px(22), py(12.162f), px(22), py(12.473f));
            nvgLineTo(vg, px(22), py(18));
            nvgBezierTo(vg, px(22), py(19.105f), px(21.105f), py(20), px(20), py(20));
            nvgLineTo(vg, px(4), py(20));
            nvgBezierTo(vg, px(2.895f), py(20), px(2), py(19.105f), px(2), py(18));
            nvgLineTo(vg, px(2), py(12.473f));
            nvgBezierTo(vg, px(2), py(12.162f), px(2.073f), py(11.855f), px(2.212f), py(11.577f));
            nvgClosePath(vg);
            nvgMoveTo(vg, px(2.054f), py(12.013f));
            nvgLineTo(vg, px(21.946f), py(12.013f));
            nvgMoveTo(vg, px(6), py(16));
            nvgLineTo(vg, px(6.01f), py(16));
            nvgMoveTo(vg, px(10), py(16));
            nvgLineTo(vg, px(10.01f), py(16));
        }

        nvgStrokeWidth(vg, 2.0f * scale);
        nvgLineCap(vg, NVG_ROUND);
        nvgLineJoin(vg, NVG_ROUND);
        nvgStrokeColor(vg, nvgRGB(77, 218, 130));
        nvgStroke(vg);
    }

  private:
    SubscriptionIcon icon_;
};

brls::Box* MakeSubscriptionChip(SubscriptionIcon icon, brls::Label*& value_label)
{
    auto* chip = new brls::Box(brls::Axis::ROW);
    chip->setHeight(34);
    chip->setShrink(0.0f);
    chip->setAlignItems(brls::AlignItems::CENTER);
    chip->setPadding(5, 10, 5, 5);
    chip->setMarginRight(6);
    chip->setCornerRadius(17);
    chip->setBorderThickness(1.5f);
    chip->setBorderColor(nvgRGB(38, 208, 103));
    chip->setBackgroundColor(nvgRGBA(8, 28, 17, 170));

    chip->addView(new SubscriptionIconView(icon));

    value_label = new brls::Label();
    value_label->setFontSize(12);
    value_label->setSingleLine(true);
    value_label->setShrink(0.0f);
    value_label->setTextColor(nvgRGB(234, 255, 241));
    chip->addView(value_label);
    return chip;
}

} // namespace

TopBarFrame::TopBarFrame()
    : brls::Box(brls::Axis::COLUMN)
{
    setGrow(1.0f);
    setBackgroundColor(nvgRGB(12, 13, 16));

    // Header container
    header_container_ = new brls::Box(brls::Axis::ROW);
    header_container_->setHeight(76);
    header_container_->setAlignItems(brls::AlignItems::CENTER);
    header_container_->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    header_container_->setPadding(0, 28, 0, 28);
    header_container_->setBackgroundColor(nvgRGB(9, 10, 12));
    
    addView(header_container_);

    auto* brand = new brls::Box(brls::Axis::ROW);
    brand->setWidth(420);
    brand->setShrink(0.0f);
    brand->setAlignItems(brls::AlignItems::CENTER);
    brand->setJustifyContent(brls::JustifyContent::FLEX_START);

    auto* brand_mark = new brls::Image();
    brand_mark->setWidth(66);
    brand_mark->setHeight(40);
    brand_mark->setMarginRight(10);
    brand_mark->setScalingType(brls::ImageScalingType::FIT);
    brand_mark->setImageFromRes("img/opennow-logo-mark.png");
    brand->addView(brand_mark);

    auto* brand_name = new brls::Label();
    brand_name->setText("OpenNOW");
    brand_name->setFontSize(21);
    brand_name->setTextColor(nvgRGB(248, 248, 248));
    brand->addView(brand_name);
    header_container_->addView(brand);

    tabs_container_ = new brls::Box(brls::Axis::ROW);
    tabs_container_->setGrow(1.0f);
    tabs_container_->setAlignItems(brls::AlignItems::CENTER);
    tabs_container_->setJustifyContent(brls::JustifyContent::CENTER);
    header_container_->addView(tabs_container_);

    auto* status_container = new brls::Box(brls::Axis::ROW);
    status_container->setWidth(420);
    status_container->setHeight(54);
    status_container->setShrink(0.0f);
    status_container->setAlignItems(brls::AlignItems::CENTER);
    status_container->setJustifyContent(brls::JustifyContent::FLEX_END);
    header_container_->addView(status_container);

    subscription_container_ = new brls::Box(brls::Axis::ROW);
    subscription_container_->setShrink(0.0f);
    subscription_container_->setAlignItems(brls::AlignItems::CENTER);
    subscription_container_->setVisibility(brls::Visibility::GONE);
    subscription_container_->addView(MakeSubscriptionChip(
        SubscriptionIcon::Timer, time_remaining_label_));
    subscription_container_->addView(MakeSubscriptionChip(
        SubscriptionIcon::HardDrive, storage_remaining_label_));
    status_container->addView(subscription_container_);

    account_container_ = new brls::Box(brls::Axis::ROW);
    account_container_->setHeight(54);
    account_container_->setShrink(0.0f);
    account_container_->setMarginLeft(3);
    account_container_->setAlignItems(brls::AlignItems::CENTER);
    account_container_->setJustifyContent(brls::JustifyContent::FLEX_END);
    status_container->addView(account_container_);

    avatar_image_ = new brls::Image();
    avatar_image_->setWidth(34);
    avatar_image_->setHeight(34);
    avatar_image_->setShrink(0.0f);
    avatar_image_->setCornerRadius(9);
    avatar_image_->setScalingType(brls::ImageScalingType::FILL);
    avatar_image_->setMarginRight(8);
    avatar_image_->setVisibility(brls::Visibility::GONE);
    account_container_->addView(avatar_image_);

    auto* account_labels = new brls::Box(brls::Axis::COLUMN);
    account_labels->setShrink(0.0f);
    account_labels->setJustifyContent(brls::JustifyContent::CENTER);
    account_name_label_ = new brls::Label();
    account_name_label_->setText("Guest");
    account_name_label_->setFontSize(13);
    account_name_label_->setSingleLine(true);
    account_name_label_->setShrink(0.0f);
    account_name_label_->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    account_name_label_->setTextColor(nvgRGB(236, 236, 239));
    account_labels->addView(account_name_label_);
    account_detail_label_ = new brls::Label();
    account_detail_label_->setText("No account");
    account_detail_label_->setFontSize(10);
    account_detail_label_->setSingleLine(true);
    account_detail_label_->setShrink(0.0f);
    account_detail_label_->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    account_detail_label_->setTextColor(nvgRGB(112, 119, 130));
    account_labels->addView(account_detail_label_);
    account_container_->addView(account_labels);

    // Divider
    auto* divider = new brls::Rectangle();
    divider->setHeight(1);
    divider->setColor(nvgRGBA(255, 255, 255, 18));
    addView(divider);

    // Content container
    content_container_ = new brls::Box(brls::Axis::COLUMN);
    content_container_->setGrow(1.0f);
    addView(content_container_);

    registerAction("Previous Tab", brls::BUTTON_LB, [this](brls::View* view) {
        if (tabs_.empty()) return false;
        int prev = active_tab_index_ - 1;
        if (prev < 0) prev = tabs_.size() - 1;
        SelectTab(prev);
        return true;
    }, true, false);

    registerAction("Next Tab", brls::BUTTON_RB, [this](brls::View* view) {
        if (tabs_.empty()) return false;
        int next = active_tab_index_ + 1;
        if (next >= (int)tabs_.size()) next = 0;
        SelectTab(next);
        return true;
    }, true, false);
}

void TopBarFrame::draw(NVGcontext* vg, float x, float y, float width, float height,
                       brls::Style style, brls::FrameContext* ctx)
{
    UpdateStatusBar();
    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

TopBarFrame::~TopBarFrame()
{
    if (active_content_)
    {
        content_container_->removeView(active_content_, false);
        active_content_ = nullptr;
    }
    for (auto& tab : tabs_)
    {
        delete tab.content;
        tab.content = nullptr;
    }
}

void TopBarFrame::addTab(const std::string& label, TabViewCreator creator)
{
    TabInfo info;
    info.label = label;
    info.creator = creator;

    auto* tab_box = new brls::Box(brls::Axis::COLUMN);
    tab_box->setAlignItems(brls::AlignItems::CENTER);
    tab_box->setJustifyContent(brls::JustifyContent::FLEX_START);
    tab_box->setMarginLeft(6);
    tab_box->setMarginRight(6);
    tab_box->setHeight(62);
    tab_box->setPadding(0, 18, 0, 18);
    tab_box->setCornerRadius(6);
    tab_box->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
    tab_box->setFocusable(true);

    auto* text = new brls::Label();
    text->setText(Tr(label));
    text->setFontSize(18);
    text->setTextColor(nvgRGB(128, 133, 143));

    auto* label_container = new brls::Box(brls::Axis::ROW);
    label_container->setHeight(57);
    label_container->setAlignItems(brls::AlignItems::CENTER);
    label_container->setJustifyContent(brls::JustifyContent::CENTER);
    label_container->addView(text);
    tab_box->addView(label_container);

    auto* underline_container = new brls::Box(brls::Axis::ROW);
    underline_container->setWidth(66);
    underline_container->setHeight(5);
    underline_container->setAlignItems(brls::AlignItems::FLEX_END);

    auto* underline = new brls::Rectangle();
    underline->setWidth(66);
    underline->setHeight(5);
    underline->setColor(nvgRGBA(0, 0, 0, 0));
    underline_container->addView(underline);

    tab_box->addView(underline_container);

    info.header_label = text;
    info.underline = underline;
    info.tab_box = tab_box;

    int index = tabs_.size();
    
    tab_box->registerClickAction([this, index](brls::View* view) {
        SelectTab(index);
        return true;
    });

    // Provide visual feedback for focus
    tab_box->setFocusSound(brls::SOUND_FOCUS_CHANGE);

    tabs_container_->addView(tab_box);
    tabs_.push_back(info);

    if (tabs_.size() == 1) {
        SelectTab(0);
    }
}

void TopBarFrame::focusTab(int position)
{
    if (position >= 0 && position < (int)tabs_.size()) {
        SelectTab(position);
    }
}

void TopBarFrame::onFocusGained()
{
    brls::Box::onFocusGained();
    if (active_content_) {
        brls::Application::giveFocus(active_content_);
    }
}

void TopBarFrame::SelectTab(int index)
{
    if (index < 0 || index >= (int)tabs_.size()) return;
    if (active_tab_index_ == index && active_content_ != nullptr) return;

    active_tab_index_ = index;
    UpdateStatusBar(true);

    for (size_t i = 0; i < tabs_.size(); ++i) {
        if ((int)i == index) {
            tabs_[i].header_label->setTextColor(nvgRGB(255, 255, 255));
            tabs_[i].underline->setColor(nvgRGB(77, 218, 130));
            tabs_[i].tab_box->setBackgroundColor(nvgRGBA(77, 218, 130, 22));
        } else {
            tabs_[i].header_label->setTextColor(nvgRGB(128, 133, 143));
            tabs_[i].underline->setColor(nvgRGBA(0, 0, 0, 0));
            tabs_[i].tab_box->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
        }
    }

    if (active_content_) {
        content_container_->removeView(active_content_, false);
        active_content_ = nullptr;
    }

    brls::View* new_content = tabs_[index].content;
    if (!new_content)
    {
        new_content = tabs_[index].creator();
        tabs_[index].content = new_content;
    }
    if (new_content) {
        new_content->setGrow(1.0f);
        content_container_->addView(new_content);
        active_content_ = new_content;
        brls::Application::giveFocus(new_content);
    }
}

void TopBarFrame::UpdateStatusBar(bool force)
{
    if (!account_name_label_ || !account_detail_label_)
        return;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_status_update_);
    if (!force && !ui::ShouldRefreshStatus(elapsed, !displayed_status_.empty()))
        return;
    last_status_update_ = now;

    const auto& state = AppState::Instance();
    if (!state.HasSession())
    {
        avatar_image_->setVisibility(brls::Visibility::GONE);
        subscription_container_->setVisibility(brls::Visibility::GONE);
        const std::string name = Tr("Guest");
        const std::string detail = Tr("No account");
        const std::string status = name + "\n" + detail;
        if (status != displayed_status_)
        {
            account_name_label_->setText(name);
            account_detail_label_->setText(detail);
            account_detail_label_->setTextColor(membership::TextColor(""));
            displayed_status_ = status;
        }
        return;
    }

    const AuthSession& session = *state.session();
    const std::string tier = membership::DisplayLabel(
        session.user.membership_tier, session.user.membership_tier_verified);
    const std::string name =
        session.user.display_name.empty() ? Tr("GeForce NOW account") : session.user.display_name;
    const std::string detail = Tr(tier);
    const std::string time = session.subscription.available
        ? subscription::FormatTimeRemaining(session.subscription)
        : "--";
    const std::string storage = session.subscription.available
        ? (session.subscription.has_storage
            ? subscription::FormatStorageRemaining(session.subscription)
            : Tr("None"))
        : "--";
    const std::string status = name + "\n" + detail + "\n" + time + "\n" + storage;
    if (status != displayed_status_)
    {
        account_name_label_->setText(name);
        account_detail_label_->setText(detail);
        account_detail_label_->setTextColor(membership::TextColor(tier));
        time_remaining_label_->setText(time);
        storage_remaining_label_->setText(storage);
        subscription_container_->setVisibility(brls::Visibility::VISIBLE);
        displayed_status_ = status;
    }

    const std::string avatar_url = ResolveAvatarUrl(session.user);
    if (!avatar_url.empty())
    {
        avatar_image_->setVisibility(brls::Visibility::VISIBLE);
        if (avatar_url != displayed_avatar_url_)
        {
            SetCachedAvatarImage(avatar_image_, avatar_url);
            displayed_avatar_url_ = avatar_url;
        }
    }
    else
    {
        avatar_image_->setVisibility(brls::Visibility::GONE);
    }
}

} // namespace opennow
