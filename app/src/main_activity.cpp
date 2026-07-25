#include "main_activity.hpp"

#include "app_state.hpp"
#include "gfn_client.hpp"
#include "home_shortcut_policy.hpp"
#include "main_tabs_view.hpp"
#include "localization.hpp"
#include "ui_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace opennow
{
namespace
{

class StartupPulseView final : public brls::View
{
  public:
    StartupPulseView()
    {
        setWidth(76);
        setHeight(76);
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override
    {
        (void)style;
        (void)ctx;
        constexpr float kPi = 3.14159265358979323846f;
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const float cx = x + width * 0.5f;
        const float cy = y + height * 0.5f;
        const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(seconds * 3.0));

        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, 28.0f + pulse * 4.0f);
        nvgFillColor(vg, nvgRGBA(77, 218, 130, 18 + static_cast<int>(pulse * 22.0f)));
        nvgFill(vg);

        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, 21.0f);
        nvgStrokeWidth(vg, 5.0f);
        nvgStrokeColor(vg, nvgRGB(35, 43, 48));
        nvgStroke(vg);

        const float angle = static_cast<float>(
            std::fmod(seconds * 3.2, static_cast<double>(kPi) * 2.0));
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, 21.0f, angle, angle + kPi * 1.42f, NVG_CW);
        nvgStrokeWidth(vg, 5.0f);
        nvgLineCap(vg, NVG_ROUND);
        nvgStrokeColor(vg, nvgRGB(77, 218, 130));
        nvgStroke(vg);

        const float head_angle = angle + kPi * 1.42f;
        nvgBeginPath(vg);
        nvgCircle(vg, cx + std::cos(head_angle) * 21.0f,
                  cy + std::sin(head_angle) * 21.0f, 3.4f);
        nvgFillColor(vg, nvgRGB(123, 242, 166));
        nvgFill(vg);
    }
};

class StartupGateView final : public brls::Box
{
  public:
    explicit StartupGateView(
        std::optional<shortcut::LaunchRequest> launch_request)
        : brls::Box(brls::Axis::COLUMN)
        , launch_request_(std::move(launch_request))
    {
        setGrow(1.0f);
        setAlignItems(brls::AlignItems::CENTER);
        setJustifyContent(brls::JustifyContent::CENTER);
        setBackgroundColor(nvgRGB(10, 12, 15));

        GfnClient client;
        AuthSession saved;
        if (!client.LoadSavedSession(saved))
        {
            AppState::Instance().MarkSessionLoaded();
            ShowMainTabs();
            return;
        }

        BuildStatusView(saved);
        BeginAuthentication(std::move(client), std::move(saved));
    }

    ~StartupGateView() override
    {
        alive_->store(false);
    }

  private:
    void BuildStatusView(const AuthSession& saved)
    {
        auto* brand = new brls::Label();
        brand->setText("OpenNOW");
        brand->setFontSize(30);
        brand->setTextColor(nvgRGB(77, 218, 130));
        brand->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        brand->setMarginBottom(34);
        addView(brand);

        auto* card = new brls::Box(brls::Axis::COLUMN);
        card->setWidth(620);
        card->setPadding(28, 38, 30, 38);
        card->setAlignItems(brls::AlignItems::CENTER);
        card->setBackgroundColor(nvgRGB(18, 21, 25));
        card->setCornerRadius(18);

        auto* eyebrow = new brls::Label();
        const std::string provider = saved.provider.display_name.empty()
            ? "GEFORCE NOW" : saved.provider.display_name;
        eyebrow->setText(Tr("SAVED ACCOUNT") + "  /  " + provider);
        eyebrow->setFontSize(13);
        eyebrow->setTextColor(nvgRGB(77, 218, 130));
        eyebrow->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        eyebrow->setMarginBottom(8);
        card->addView(eyebrow);

        auto* account = new brls::Label();
        account->setText(saved.user.display_name.empty() ? saved.user.email : saved.user.display_name);
        account->setFontSize(25);
        account->setTextColor(nvgRGB(242, 246, 248));
        account->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        account->setMarginBottom(14);
        card->addView(account);

        card->addView(new StartupPulseView());

        status_label_ = new brls::Label();
        status_label_->setText(Tr("Checking saved sign-in"));
        status_label_->setFontSize(21);
        status_label_->setTextColor(nvgRGB(238, 242, 245));
        status_label_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        status_label_->setMarginTop(8);
        status_label_->setMarginBottom(7);
        card->addView(status_label_);

        detail_label_ = new brls::Label();
        detail_label_->setText(Tr("Validating the session before loading your library."));
        detail_label_->setFontSize(15);
        detail_label_->setTextColor(nvgRGB(145, 153, 164));
        detail_label_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        card->addView(detail_label_);
        addView(card);
    }

    void SetStatus(std::string title, std::string detail)
    {
        const auto alive = alive_;
        brls::sync([this, alive, title = std::move(title), detail = std::move(detail)]() {
            if (!alive->load() || finished_)
                return;
            status_label_->setText(Tr(title));
            detail_label_->setText(Tr(detail));
        });
    }

    void Complete(AuthSession session, const std::string& notification)
    {
        const auto alive = alive_;
        brls::sync([this, alive, session = std::move(session), notification]() mutable {
            if (!alive->load() || finished_)
                return;
            AppState::Instance().SetSession(std::move(session));
            ShowMainTabs();
            if (!notification.empty())
                brls::Application::notify(notification);
        });
    }

    void BeginAuthentication(GfnClient client, AuthSession saved)
    {
        const auto alive = alive_;
        brls::async([this, alive, client = std::move(client), saved = std::move(saved)]() mutable {
            try
            {
                SetStatus("Checking saved session",
                          "Refreshing authorization and verifying account access.");
                AuthSession verified = client.RecoverSavedSession(saved, true);

                if (!alive->load())
                    return;
                SetStatus("Account verified", "Loading OpenNOW and your saved profile.");
                verified.reauthentication_required = false;
                Complete(std::move(verified), "GeForce NOW account is ready");
            }
            catch (const ReauthenticationRequired& ex)
            {
                if (!alive->load())
                    return;
                saved.reauthentication_required = true;
                Complete(std::move(saved),
                         "The provider requires a new QR code sign-in from Settings");
                brls::Logger::warning("Startup sign-in requires QR reconnection: {}", ex.what());
            }
            catch (const std::exception& ex)
            {
                if (!alive->load())
                    return;
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                const bool expired = saved.tokens.expires_at_ms > 0 &&
                    saved.tokens.expires_at_ms <= now_ms;
                saved.reauthentication_required = saved.reauthentication_required || expired;
                Complete(std::move(saved),
                         expired
                            ? "Automatic sign-in could not finish; it will retry in OpenNOW"
                            : "Session check is temporarily unavailable; using the saved account");
                brls::Logger::warning("Startup authentication check failed: {}", ex.what());
            }
        }, false);
    }

    void ShowMainTabs()
    {
        if (finished_)
            return;
        finished_ = true;
        clearViews();
        setAlignItems(brls::AlignItems::STRETCH);
        setJustifyContent(brls::JustifyContent::FLEX_START);
        setPadding(0);
        auto* tabs = new MainTabsView();
        tabs->setWidth(brls::Application::windowWidth);
        tabs->setHeight(brls::Application::windowHeight);
        tabs->setGrow(1.0f);
        addView(tabs);
        invalidate();
        brls::Application::giveFocus(tabs);
        const auto alive = alive_;
        brls::sync([this, alive] {
            if (alive->load())
                MaybeLaunchShortcut();
        });
    }

    void MaybeLaunchShortcut()
    {
        if (!launch_request_)
            return;

        const shortcut::LaunchRequest request = std::move(*launch_request_);
        launch_request_.reset();
        const auto& session = AppState::Instance().session();
        if (!session)
        {
            ShowError(
                "Shortcut needs sign-in",
                "OpenNOW could not start " + request.title +
                " because no saved GeForce NOW account is connected. Sign in, "
                "then open the shortcut again.");
            return;
        }
        if (session->reauthentication_required)
        {
            ShowError(
                "Shortcut needs sign-in",
                "Reconnect the saved GeForce NOW account, then open the " +
                request.title + " shortcut again.");
            return;
        }

        GfnClient client;
        LaunchSessionDialog(
            client, *session, request.launch_app_id, request.title,
            request.store, request.title, request.game_id, request.image_url);
    }

    brls::Label* status_label_ = nullptr;
    brls::Label* detail_label_ = nullptr;
    std::shared_ptr<std::atomic_bool> alive_ = std::make_shared<std::atomic_bool>(true);
    std::optional<shortcut::LaunchRequest> launch_request_;
    bool finished_ = false;
};

} // namespace

MainActivity::MainActivity(
    std::optional<shortcut::LaunchRequest> launch_request)
    : brls::Activity(new StartupGateView(std::move(launch_request)))
{
}

} // namespace opennow
