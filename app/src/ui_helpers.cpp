#include "ui_helpers.hpp"

#include "app_state.hpp"
#include "cover_image_cache.hpp"
#include "play_history.hpp"
#include "session_error_policy.hpp"
#include "localization.hpp"
#include <borealis.hpp>
#include <switch.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include "StreamView.hpp"

namespace opennow
{
namespace
{

struct LaunchSessionState
{
    std::atomic<bool> running {true};
    std::mutex mutex;
    std::string session_id;
};

class LaunchAnimationView final : public brls::View
{
  public:
    LaunchAnimationView()
    {
        setWidth(620);
        setHeight(168);
    }

    void SetState(int source_stage, float progress)
    {
        visual_stage_ = source_stage <= 1 ? 0 : (source_stage == 2 ? 1 : 2);
        progress_ = std::clamp(progress, 0.04f, 0.98f);
        invalidate();
    }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override
    {
        (void)style;
        (void)ctx;
        constexpr float kPi = 3.14159265358979323846f;
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(seconds * 3.2));
        static constexpr const char* kLabels[] = {"Queue", "Setup", "Ready"};
        const float rail_y = y + 42.0f;
        const float first_x = x + 76.0f;
        const float gap = (width - 152.0f) * 0.5f;

        nvgBeginPath(vg);
        nvgRect(vg, first_x, rail_y - 2.0f, gap * 2.0f, 4.0f);
        nvgFillColor(vg, nvgRGB(38, 42, 48));
        nvgFill(vg);

        float completed_width = visual_stage_ * gap;
        if (visual_stage_ < 2)
            completed_width += gap * std::clamp((progress_ - visual_stage_ * 0.32f) / 0.64f, 0.0f, 0.90f);
        else
            completed_width = gap * 2.0f;
        nvgBeginPath(vg);
        nvgRect(vg, first_x, rail_y - 2.0f, completed_width, 4.0f);
        nvgFillColor(vg, nvgRGB(0, 200, 215));
        nvgFill(vg);

        nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        for (int i = 0; i < 3; ++i)
        {
            const float cx = first_x + gap * i;
            const bool complete = i < visual_stage_;
            const bool active = i == visual_stage_;
            if (active)
            {
                nvgBeginPath(vg);
                nvgCircle(vg, cx, rail_y, 29.0f + pulse * 3.0f);
                nvgFillColor(vg, nvgRGBA(0, 200, 215, 25 + static_cast<int>(pulse * 22.0f)));
                nvgFill(vg);
            }
            nvgBeginPath(vg);
            nvgCircle(vg, cx, rail_y, 22.0f);
            nvgFillColor(vg, complete || active ? nvgRGB(0, 200, 215) : nvgRGB(24, 28, 33));
            nvgFill(vg);
            nvgStrokeWidth(vg, 2.0f);
            nvgStrokeColor(vg, complete || active ? nvgRGB(100, 231, 239) : nvgRGB(43, 48, 55));
            nvgStroke(vg);

            nvgFontSize(vg, 17.0f);
            nvgFillColor(vg, complete || active ? nvgRGB(8, 35, 22) : nvgRGB(95, 101, 111));
            const std::string number = std::to_string(i + 1);
            nvgText(vg, cx, rail_y, number.c_str(), nullptr);
            nvgFontSize(vg, 12.0f);
            nvgFillColor(vg, complete || active ? nvgRGB(228, 235, 232) : nvgRGB(91, 96, 105));
            nvgText(vg, cx, rail_y + 42.0f, kLabels[i], nullptr);
        }

        const float spinner_x = x + width * 0.5f;
        const float spinner_y = y + 137.0f;
        const float angle = static_cast<float>(
            std::fmod(seconds * 3.0, static_cast<double>(kPi) * 2.0));
        nvgBeginPath(vg);
        nvgCircle(vg, spinner_x, spinner_y, 18.0f);
        nvgStrokeWidth(vg, 5.0f);
        nvgStrokeColor(vg, nvgRGB(35, 43, 48));
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgArc(vg, spinner_x, spinner_y, 18.0f, angle,
               angle + kPi * 1.42f, NVG_CW);
        nvgStrokeWidth(vg, 5.0f);
        nvgLineCap(vg, NVG_ROUND);
        nvgStrokeColor(vg, nvgRGB(0, 200, 215));
        nvgStroke(vg);
        const float head_angle = angle + kPi * 1.42f;
        nvgBeginPath(vg);
        nvgCircle(vg, spinner_x + std::cos(head_angle) * 18.0f,
                  spinner_y + std::sin(head_angle) * 18.0f, 3.2f);
        nvgFillColor(vg, nvgRGB(118, 234, 241));
        nvgFill(vg);
    }

  private:
    float progress_ = 0.08f;
    int visual_stage_ = 0;
};

void SetLaunchProgress(
    LaunchAnimationView* animation,
    brls::Label* stage_label,
    brls::Label* detail_label,
    int stage,
    const std::string& title,
    const std::string& detail,
    float progress)
{
    if (animation)
        animation->SetState(stage, progress);
    if (stage_label)
    {
        stage_label->setText(Tr(title));
        const bool queue_position = title.rfind("Position in queue:", 0) == 0;
        stage_label->setFontSize(queue_position ? 36 : 27);
        stage_label->setTextColor(queue_position
            ? nvgRGB(0, 200, 215)
            : nvgRGB(238, 242, 245));
    }
    if (detail_label)
    {
        detail_label->setText(Tr(detail));
        detail_label->setFontSize(title.rfind("Position in queue:", 0) == 0 ? 18 : 16);
    }
}

std::string SessionStatusText(const SessionInfo& info)
{
    if (info.app_patching)
        return "NVIDIA is updating this game...\nThe session will start automatically when patching finishes.";

    if (info.status == 0)
        return "In queue... Position: " + std::to_string(info.queue_position);

    if (info.status == 1)
    {
        if (info.queue_position > 0)
            return "Setting up rig... Position: " + std::to_string(info.queue_position);

        return "Setting up rig... Please wait.";
    }

    if (info.status == 6)
        return "Waiting for NVIDIA session ads or confirmation...";

    return "Waiting... Status: " + std::to_string(info.status);
}

} // namespace

void ShowDialog(const std::string& title, const std::string& body)
{
    auto* dialog = new brls::Dialog(Tr(title) + "\n\n" + Tr(body));
    dialog->addButton(Tr("Close"), [] {});
    dialog->setCancelable(true);
    dialog->open();
}

void ShowError(const std::string& title, const std::string& body)
{
    auto* dialog = new brls::Dialog(Tr(title) + "\n\n" + Tr(body));
    dialog->addButton(Tr("Close"), [] {});
    dialog->setCancelable(false);
    dialog->open();
}

void LaunchSessionDialog(const GfnClient& client, const AuthSession& auth,
                         const std::string& launch_app_id, const std::string& title,
                         const std::string& launch_store,
                         const std::string& internal_title,
                         const std::string& history_game_id,
                         const std::string& image_url)
{
    auto* box = new brls::Box(brls::Axis::COLUMN);
    box->setWidth(720);
    box->setPadding(24, 36, 18, 36);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setBackgroundColor(nvgRGBA(0, 0, 0, 0));

    auto* game_header = new brls::Box(brls::Axis::ROW);
    game_header->setWidth(620);
    game_header->setHeight(96);
    game_header->setAlignItems(brls::AlignItems::CENTER);
    game_header->setMarginBottom(6);

    auto* cover = new brls::Image();
    cover->setWidth(82);
    cover->setHeight(82);
    cover->setCornerRadius(12);
    cover->setScalingType(brls::ImageScalingType::FILL);
    cover->setMarginRight(20);
    SetCachedCoverImage(cover, image_url);
    game_header->addView(cover);

    auto* game_copy = new brls::Box(brls::Axis::COLUMN);
    game_copy->setGrow(1.0f);
    auto* eyebrow = new brls::Label();
    eyebrow->setText(Tr("Now loading"));
    eyebrow->setFontSize(13);
    eyebrow->setTextColor(nvgRGB(0, 200, 215));
    eyebrow->setMarginBottom(5);
    game_copy->addView(eyebrow);
    auto* heading = new brls::Label();
    heading->setText(title);
    heading->setFontSize(27);
    heading->setTextColor(nvgRGB(245, 248, 250));
    heading->setMarginBottom(5);
    game_copy->addView(heading);
    auto* store_label = new brls::Label();
    store_label->setText(launch_store.empty() ? "GeForce NOW" : launch_store);
    store_label->setFontSize(14);
    store_label->setTextColor(nvgRGB(145, 151, 162));
    game_copy->addView(store_label);
    game_header->addView(game_copy);
    box->addView(game_header);

    auto* animation = new LaunchAnimationView();
    animation->setMarginBottom(0);
    box->addView(animation);

    auto* stage_label = new brls::Label();
    stage_label->setText(Tr("Checking your NVIDIA account"));
    stage_label->setFontSize(27);
    stage_label->setTextColor(nvgRGB(238, 242, 245));
    stage_label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    stage_label->setMarginBottom(6);
    box->addView(stage_label);

    auto* detail_label = new brls::Label();
    detail_label->setText(Tr("Validating the saved session before requesting a cloud rig."));
    detail_label->setFontSize(16);
    detail_label->setTextColor(nvgRGB(151, 159, 170));
    detail_label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    detail_label->setMarginBottom(4);
    box->addView(detail_label);

    SetLaunchProgress(animation, stage_label, detail_label, 0,
                      "Checking your NVIDIA account",
                      "Validating the saved session before requesting a cloud rig.", 0.08f);

    auto* dialog = new brls::Dialog(box);
    dialog->setCancelable(false);

    GfnClient bg_client = client;
    AuthSession bg_auth = auth;
    std::string bg_app_id = launch_app_id;
    std::string bg_store = launch_store;
    std::string bg_internal_title = internal_title.empty() ? title : internal_title;
    std::string bg_history_game_id = history_game_id.empty() ? launch_app_id : history_game_id;

    auto launch_state = std::make_shared<LaunchSessionState>();

    dialog->addButton(Tr("Cancel session"), [launch_state, bg_client, bg_auth]() {
        launch_state->running = false;
        std::string session_id;
        {
            std::lock_guard<std::mutex> lock(launch_state->mutex);
            session_id = launch_state->session_id;
        }
        if (!session_id.empty()) {
            brls::async([bg_client, bg_auth, sid = std::move(session_id)]() mutable {
                try { bg_client.StopSession(bg_auth, sid); } catch (...) {}
            });
        }
    });
    dialog->open();

    brls::async([dialog, animation, stage_label, detail_label,
                 bg_client, bg_auth, bg_app_id, bg_store, bg_internal_title,
                 bg_history_game_id, launch_state]() mutable {
        auto post_progress = [animation, stage_label, detail_label, launch_state](
            int stage, std::string title, std::string detail, float progress) {
            brls::sync([animation, stage_label, detail_label, launch_state,
                        stage, title = std::move(title), detail = std::move(detail), progress]() {
                if (!launch_state->running)
                    return;
                SetLaunchProgress(animation, stage_label, detail_label,
                                  stage, title, detail, progress);
            });
        };
        try {
            post_progress(0, "Checking your NVIDIA account",
                          "Renewing authorization and checking previous sessions.", 0.10f);
            SessionInfo info;
            try {
                bg_client.CleanupStaleCloudSession(bg_auth);
                post_progress(1, "Requesting a cloud rig",
                              "GeForce NOW is allocating hardware for your game.", 0.24f);
                info = bg_client.StartSession(bg_auth, bg_app_id, bg_store, bg_internal_title);
            }
            catch (const ReauthenticationRequired&)
            {
                post_progress(0, "Signing in automatically",
                              "The saved session expired. Using your remembered NVIDIA account.", 0.14f);
                bg_auth = bg_client.ReauthenticateWithSavedCredentials(
                    bg_auth,
                    [post_progress](const std::string& status) {
                        post_progress(0, "Confirming your NVIDIA account", status, 0.18f);
                    },
                    [launch_state]() { return !launch_state->running.load(); });
                if (!launch_state->running)
                    return;
                post_progress(1, "Account restored - requesting a rig",
                              "Quick sign-in completed successfully.", 0.26f);
                bg_client.CleanupStaleCloudSession(bg_auth);
                info = bg_client.StartSession(bg_auth, bg_app_id, bg_store, bg_internal_title);
            }
            {
                std::lock_guard<std::mutex> lock(launch_state->mutex);
                launch_state->session_id = info.session_id;
            }

            int unknown_status_polls = 0;
            
            while (info.status != 2 && info.status != 3) {
                if (info.status > 3 && info.status != 6) {
                    throw std::runtime_error("Session ended or aborted by NVIDIA.");
                }

                if (info.status < 0 && ++unknown_status_polls >= 6) {
                    throw std::runtime_error("NVIDIA returned an unknown session status for too long.");
                }
                
                const float queue_progress = info.status == 0 ? 0.34f : 0.48f;
                std::string queue_title;
                std::string queue_detail;
                if (info.queue_position > 0)
                {
                    queue_title = "Position in queue: " + std::to_string(info.queue_position);
                    queue_detail = info.status == 0
                        ? "Waiting for an available cloud rig"
                        : "Preparing your cloud rig";
                }
                else if (info.status == 0)
                {
                    queue_title = "Waiting in queue...";
                    queue_detail = "Your cloud rig will start automatically when it is ready.";
                }
                else
                {
                    queue_title = info.app_patching ? "Updating the game" : "Preparing your cloud rig";
                    queue_detail = SessionStatusText(info);
                }
                post_progress(1, queue_title, queue_detail, queue_progress);

                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (!launch_state->running) return;
                info = bg_client.PollSession(bg_auth, info.session_id);
            }

            if (info.signaling_url.empty()) {
                throw std::runtime_error("Session is ready, but NVIDIA did not return a signaling URL.");
            }

            post_progress(2, "Connecting to the streaming server",
                          "The rig is ready. Configuring the secure network path.", 0.72f);
            brls::sync([=]() {
                if (!launch_state->running) return;
                SetLaunchProgress(animation, stage_label, detail_label, 3,
                                  "Starting the video stream",
                                  "Negotiating WebRTC and waiting for the first clean frame.", 0.86f);
                dialog->close([launch_state](){ launch_state->running = false; });

                auto& app_state = AppState::Instance();
                if (app_state.HasSession() && app_state.session()->user.user_id == bg_auth.user.user_id)
                    app_state.SetSession(bg_auth);
                const std::string played_at = CurrentUtcIsoTimestamp();
                RecordGamePlayed(bg_history_game_id, bg_internal_title, played_at);
                app_state.MarkGamePlayed(bg_history_game_id, bg_internal_title, played_at);
                
                brls::Logger::info("WebRTC Stream Ready!");
                brls::Logger::info("Server IP: {}", info.server_ip);
                brls::Logger::info("Signaling URL: {}", info.signaling_url);
                brls::Logger::info("Media endpoint: {}:{}", info.media_ip, info.media_port);
                brls::Logger::info("ICE servers: {}", info.ice_servers.size());
                
                std::string jwt_token = bg_auth.tokens.id_token.empty() ? bg_auth.tokens.access_token : bg_auth.tokens.id_token;
                brls::Application::pushActivity(new brls::Activity(StreamView::create(
                    info.signaling_url,
                    jwt_token,
                    info.session_id,
                    info.media_ip,
                    info.media_port,
                    info.ice_servers,
                    bg_client,
                    bg_auth,
                    bg_internal_title)));
            });

        } catch (const std::exception& e) {
            const session_error::Presentation error = session_error::Present(e.what());
            brls::sync([=]() {
                if (!launch_state->running) return;
                animation->SetState(0, 0.04f);
                stage_label->setText(Tr(error.title));
                stage_label->setFontSize(24);
                stage_label->setTextColor(nvgRGB(255, 125, 132));
                detail_label->setFontSize(17);
                detail_label->setText(
                    Tr(error.body) + "\n\n" + Tr("Press B to return to the game page."));
                dialog->setCancelable(true);
            });
        }
    });
}

} // namespace opennow
