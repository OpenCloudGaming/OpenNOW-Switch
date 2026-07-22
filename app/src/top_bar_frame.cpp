#include "top_bar_frame.hpp"
#include "app_state.hpp"
#include "avatar_utils.hpp"
#include "cover_image_cache.hpp"
#include "stream_settings.hpp"
#include "localization.hpp"
#include "membership_tier_policy.hpp"
#include "ui_refresh_policy.hpp"
#include <borealis/core/application.hpp>
#include <borealis/core/theme.hpp>
#include <borealis/core/logger.hpp>

namespace opennow
{
TopBarFrame::TopBarFrame()
    : brls::Box(brls::Axis::COLUMN)
{
    setGrow(1.0f);
    setBackgroundColor(nvgRGB(18, 18, 22));

    // Header container
    header_container_ = new brls::Box(brls::Axis::ROW);
    header_container_->setHeight(76);
    header_container_->setAlignItems(brls::AlignItems::CENTER);
    header_container_->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    header_container_->setPadding(0, 32, 0, 32);
    header_container_->setBackgroundColor(nvgRGB(24, 24, 29));
    
    addView(header_container_);

    auto* brand = new brls::Box(brls::Axis::COLUMN);
    // Match the account gutter so the navigation is centered on the screen,
    // rather than merely centered in the space left between unequal sides.
    brand->setWidth(310);
    brand->setJustifyContent(brls::JustifyContent::CENTER);
    auto* brand_name = new brls::Label();
    brand_name->setText("OpenNOW");
    brand_name->setFontSize(22);
    brand_name->setTextColor(nvgRGB(77, 218, 130));
    brand->addView(brand_name);
    auto* brand_platform = new brls::Label();
    brand_platform->setText("Nintendo Switch");
    brand_platform->setFontSize(10);
    brand_platform->setTextColor(nvgRGB(112, 119, 130));
    brand->addView(brand_platform);
    header_container_->addView(brand);

    tabs_container_ = new brls::Box(brls::Axis::ROW);
    tabs_container_->setGrow(1.0f);
    tabs_container_->setAlignItems(brls::AlignItems::CENTER);
    tabs_container_->setJustifyContent(brls::JustifyContent::CENTER);
    header_container_->addView(tabs_container_);

    account_container_ = new brls::Box(brls::Axis::ROW);
    account_container_->setWidth(310);
    account_container_->setHeight(54);
    account_container_->setAlignItems(brls::AlignItems::CENTER);
    account_container_->setJustifyContent(brls::JustifyContent::FLEX_END);
    header_container_->addView(account_container_);

    avatar_image_ = new brls::Image();
    avatar_image_->setWidth(40);
    avatar_image_->setHeight(40);
    avatar_image_->setCornerRadius(20);
    avatar_image_->setScalingType(brls::ImageScalingType::FILL);
    avatar_image_->setMarginRight(10);
    avatar_image_->setVisibility(brls::Visibility::GONE);
    account_container_->addView(avatar_image_);

    auto* account_labels = new brls::Box(brls::Axis::COLUMN);
    account_labels->setWidth(255);
    account_labels->setJustifyContent(brls::JustifyContent::CENTER);
    account_name_label_ = new brls::Label();
    account_name_label_->setText("Guest");
    account_name_label_->setFontSize(15);
    account_name_label_->setSingleLine(true);
    account_name_label_->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    account_name_label_->setTextColor(nvgRGB(236, 236, 239));
    account_labels->addView(account_name_label_);
    account_detail_label_ = new brls::Label();
    account_detail_label_->setText("No account");
    account_detail_label_->setFontSize(12);
    account_detail_label_->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
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
    tab_box->setJustifyContent(brls::JustifyContent::CENTER);
    tab_box->setMarginLeft(6);
    tab_box->setMarginRight(6);
    tab_box->setHeight(50);
    tab_box->setPadding(8, 18, 8, 18);
    tab_box->setCornerRadius(8);
    tab_box->setBackgroundColor(nvgRGBA(0, 0, 0, 0));
    tab_box->setFocusable(true);

    auto* text = new brls::Label();
    text->setText(Tr(label));
    text->setFontSize(18);
    text->setTextColor(nvgRGB(128, 133, 143));
    tab_box->addView(text);

    auto* underline_container = new brls::Box(brls::Axis::ROW);
    underline_container->setHeight(3);
    underline_container->setGrow(1.0f);

    auto* underline = new brls::Rectangle();
    underline->setHeight(3);
    underline->setGrow(1.0f);
    // 100% width of the tab_box
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
    tab_box->registerAction("Select", brls::BUTTON_A, [this, index](brls::View* view) {
        SelectTab(index);
        return true;
    });

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
            tabs_[i].tab_box->setBackgroundColor(nvgRGB(31, 34, 40));
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
    const StreamSettings settings = LoadStreamSettings();
    if (!state.HasSession())
    {
        avatar_image_->setVisibility(brls::Visibility::GONE);
        const std::string name = Tr("Guest");
        const std::string detail = Tr("No account") + "  /  " + Tr(settings.label);
        const std::string status = name + "\n" + detail;
        if (status != displayed_status_)
        {
            account_name_label_->setText(name);
            account_detail_label_->setText(detail);
            displayed_status_ = status;
        }
        return;
    }

    const AuthSession& session = *state.session();
    const std::string tier = membership::DisplayLabel(
        session.user.membership_tier, session.user.membership_tier_verified);
    const std::string name =
        session.user.display_name.empty() ? Tr("GeForce NOW account") : session.user.display_name;
    const std::string detail = Tr(tier) + "  /  " + Tr(settings.label);
    const std::string status = name + "\n" + detail;
    if (status != displayed_status_)
    {
        account_name_label_->setText(name);
        account_detail_label_->setText(detail);
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
