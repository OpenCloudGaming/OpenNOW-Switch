#include "ui_helpers.hpp"

#include "app_state.hpp"
#include "queue_view.hpp"
#include "cloud_launch_internal.hpp"
#include "cloud_launch_state.hpp"
#include "play_history.hpp"
#include "session_error_policy.hpp"
#include "localization.hpp"
#include "network_utils.hpp"
#include <borealis.hpp>
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace opennow
{
namespace
{

class QueueActivity;

struct QueueSessionState
{
    CloudLaunchState launch;
    GfnClient client;
    AuthSession auth;
    StreamSettings settings;
    std::string game_title;
    std::string title = "Checking your NVIDIA account";
    std::string detail = "Validating the saved session before requesting a cloud rig.";
    int stage = 0;
    int position = -1;
    bool minimized = false;
    bool notified = false;
    QueueActivity* activity = nullptr;
    QueueView* view = nullptr;
};

std::shared_ptr<QueueSessionState> active_queue;

void CancelQueue(const std::shared_ptr<QueueSessionState>& state);

class QueueActivity final : public brls::Activity
{
  public:
    explicit QueueActivity(std::shared_ptr<QueueSessionState> state)
        : state_(std::move(state))
    {
        auto* view = new QueueView(Display(), state_->settings,
            [this] { Dismiss(); },
            [this] { CancelQueue(state_); Dismiss(); });
        state_->view = view;
        state_->activity = this;
        state_->minimized = false;
    }

    brls::View* createContentView() override { return state_->view; }

    ~QueueActivity() override
    {
        StopDismissWatch();
        Detach();
    }

    void willDisappear(bool reset_state = false) override
    {
        if (reset_state)
            Detach();
        brls::Activity::willDisappear(reset_state);
    }

    void onResume() override
    {
        if (!dismiss_pending_ || dismiss_watch_)
            return;
        dismiss_watch_ = brls::Application::getRunLoopEvent()->subscribe([this] {
            const auto stack = brls::Application::getActivitiesStack();
            if (stack.empty() || stack.back() != this || !dismiss_pending_)
                return;
            dismiss_pending_ = false;
            brls::sync([state = state_] {
                if (state->activity)
                    state->activity->Dismiss();
            });
        });
    }

    void onPause() override { StopDismissWatch(); }

    void Dismiss(std::function<void()> callback = [] {})
    {
        StopDismissWatch();
        const auto stack = brls::Application::getActivitiesStack();
        if (!stack.empty() && stack.back() != this)
        {
            dismiss_pending_ = true;
            state_->view = nullptr;
            callback();
            return;
        }
        dismiss_pending_ = false;
        Detach();
        brls::Application::popActivity(brls::TransitionAnimation::NONE, std::move(callback));
    }

    QueueDisplayState Display() const
    {
        return {state_->game_title, state_->title, state_->detail,
                state_->position, state_->stage, !state_->launch.running()};
    }

  private:
    void StopDismissWatch()
    {
        if (!dismiss_watch_)
            return;
        brls::Application::getRunLoopEvent()->unsubscribe(*dismiss_watch_);
        dismiss_watch_.reset();
    }

    void Detach()
    {
        if (state_->activity != this)
            return;
        state_->activity = nullptr;
        state_->view = nullptr;
        state_->minimized = state_->launch.running();
    }

    std::shared_ptr<QueueSessionState> state_;
    bool dismiss_pending_ = false;
    std::optional<brls::VoidEvent::Subscription> dismiss_watch_;
};

void FinishQueue(const std::shared_ptr<QueueSessionState>& state)
{
    if (active_queue != state)
        return;
    active_queue.reset();
#ifdef __SWITCH__
    brls::Application::getPlatform()->disableScreenDimming(false);
#endif
}

void CancelQueue(const std::shared_ptr<QueueSessionState>& state)
{
    std::string session_id = state->launch.Cancel();
    FinishQueue(state);
    if (!session_id.empty()) {
        brls::async([client = state->client, auth = state->auth,
                     session_id = std::move(session_id)]() mutable {
            try { client.StopSession(auth, session_id); } catch (...) {}
        });
    }
}

void UpdateQueue(const std::shared_ptr<QueueSessionState>& state)
{
    if (state->view)
        state->view->Update({state->game_title, state->title, state->detail,
                            state->position, state->stage, !state->launch.running()});
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

int GetCurrentQueuePosition()
{
    return active_queue && active_queue->launch.running() ? active_queue->position : -1;
}

std::string GetCurrentQueueTitle()
{
    return active_queue ? active_queue->title : "Queue";
}

bool IsQueueMinimized()
{
    return active_queue && active_queue->launch.running() && active_queue->minimized;
}

void RestoreMinimizedQueueDialog()
{
    auto state = active_queue;
    if (!state || !state->launch.running() || !state->minimized || state->activity)
        return;
    brls::Application::pushActivity(new QueueActivity(state));
}

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

namespace
{

void BeginLaunchSessionDialog(const GfnClient& client, const AuthSession& auth,
                              const std::string& launch_app_id, const std::string& title,
                              const std::string& launch_store,
                              const std::string& internal_title,
                              const std::string& history_game_id,
                              const std::string& image_url)
{
    if (active_queue && active_queue->launch.running()) {
        RestoreMinimizedQueueDialog();
        brls::Application::notify(Tr("Cancel the current queue before starting another game."));
        return;
    }

    (void)image_url;

    GfnClient bg_client = client;
    AuthSession bg_auth = auth;
    std::string bg_app_id = launch_app_id;
    std::string bg_store = launch_store;
    std::string bg_internal_title = internal_title.empty() ? title : internal_title;
    std::string bg_history_game_id = history_game_id.empty() ? launch_app_id : history_game_id;

    auto launch_state = std::make_shared<QueueSessionState>();
    launch_state->client = client;
    launch_state->auth = auth;
    launch_state->settings = LoadStreamSettings();
    launch_state->game_title = title;
    active_queue = launch_state;
    const auto account_generation = AppState::Instance().session_generation();
    brls::Application::pushActivity(new QueueActivity(launch_state));
#ifdef __SWITCH__
    brls::Application::getPlatform()->disableScreenDimming(true);
#endif

    brls::async([bg_client, bg_auth, bg_app_id, bg_store, bg_internal_title,
                 bg_history_game_id, launch_state, account_generation]() mutable {
        auto post_progress = [launch_state, account_generation](
            int stage, std::string title, std::string detail, int position = -1) {
            brls::sync([launch_state, account_generation, stage,
                        title = std::move(title), detail = std::move(detail), position]() {
                if (!launch_state->launch.running())
                    return;
                if (!AppState::Instance().IsCurrentSession(account_generation)) {
                    if (launch_state->activity)
                        launch_state->activity->Dismiss();
                    CancelQueue(launch_state);
                    return;
                }
                launch_state->stage = stage;
                launch_state->title = title;
                launch_state->detail = detail;
                launch_state->position = position;
                UpdateQueue(launch_state);
                if (launch_state->minimized && !launch_state->notified && position > 0 &&
                    position <= launch_state->settings.queue_notify_threshold) {
                    launch_state->notified = true;
                    brls::Application::notify(Tr("Queue almost ready. Tap the Queue chip to return."));
                }
            });
        };
        try {
            if (!launch_state->launch.running())
                return;
            post_progress(0, "Checking your NVIDIA account",
                          "Renewing authorization and checking previous sessions.");
            bg_client.CleanupStaleCloudSession(bg_auth);
            if (!launch_state->launch.running())
                return;
            post_progress(1, "Requesting a cloud rig",
                          "GeForce NOW is allocating hardware for your game.");
            SessionInfo info = bg_client.StartSession(
                bg_auth, bg_app_id, launch_state->settings, bg_store, bg_internal_title);
            if (!launch_state->launch.Adopt(info.session_id)) {
                try { bg_client.StopSession(bg_auth, info.session_id); } catch (...) {}
                return;
            }

            int unknown_status_polls = 0;
            
            while (info.status != 2 && info.status != 3) {
                if (info.status > 3 && info.status != 6) {
                    throw std::runtime_error("Session ended or aborted by NVIDIA.");
                }

                if (info.status < 0 && ++unknown_status_polls >= 6) {
                    throw std::runtime_error("NVIDIA returned an unknown session status for too long.");
                }
                
                std::string queue_title;
                std::string queue_detail;
                if (info.app_patching)
                {
                    queue_title = "Updating the game";
                    queue_detail = SessionStatusText(info);
                }
                else if (info.status == 6)
                {
                    queue_title = "Waiting for NVIDIA confirmation";
                    queue_detail = SessionStatusText(info);
                }
                else if (info.queue_position > 0)
                {
                    queue_title = info.status == 0
                        ? "Waiting for an available cloud rig"
                        : "Preparing your cloud rig";
                    queue_detail = "Your game starts automatically when the rig is ready.";
                }
                else if (info.status == 0)
                {
                    queue_title = "Waiting in queue...";
                    queue_detail = "Your cloud rig will start automatically when it is ready.";
                }
                else
                {
                    queue_title = info.status < 0 ? "Waiting for cloud session status" : "Preparing your cloud rig";
                    queue_detail = SessionStatusText(info);
                }
                post_progress(info.status == 0 ? 1 : 2, queue_title, queue_detail, info.queue_position > 0 ? info.queue_position : -1);

                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (!launch_state->launch.running()) return;
                info = bg_client.PollSession(bg_auth, info.session_id);
            }

            if (info.signaling_url.empty()) {
                throw std::runtime_error("Session is ready, but NVIDIA did not return a signaling URL.");
            }

            post_progress(2, "Connecting to the streaming server",
                          "The rig is ready. Configuring the secure network path.");
            brls::sync([=]() {
                if (!launch_state->launch.running()) return;
                if (!AppState::Instance().IsCurrentSession(account_generation)) {
                    if (launch_state->activity)
                        launch_state->activity->Dismiss();
                    CancelQueue(launch_state);
                    return;
                }
                try {
                    launch_state->stage = 3;
                    launch_state->title = "Starting the video stream";
                    launch_state->detail = "Negotiating WebRTC and waiting for the first clean frame.";
                    UpdateQueue(launch_state);

                    auto& app_state = AppState::Instance();
                    if (app_state.IsCurrentSession(account_generation))
                        app_state.SetSession(bg_auth);
                    const std::string played_at = CurrentUtcIsoTimestamp();
                    RecordGamePlayed(bg_history_game_id, bg_internal_title, played_at);
                    app_state.MarkGamePlayed(bg_history_game_id, bg_internal_title, played_at);

                    brls::Logger::info("WebRTC Stream Ready!");
                    brls::Logger::info("Server IP: {}", info.server_ip);
                    brls::Logger::info("Media endpoint: {}:{}", info.media_ip, info.media_port);
                    brls::Logger::info("ICE servers: {}", info.ice_servers.size());

                    auto handoff = [=] {
                        if (!launch_state->launch.running())
                            return;
                        if (!AppState::Instance().IsCurrentSession(account_generation)) {
                            CancelQueue(launch_state);
                            return;
                        }
                        try {
                            FinishQueue(launch_state);
                            PresentCloudStream(info, bg_client, bg_auth, bg_internal_title, launch_state->settings);
                            launch_state->launch.TransferToStream();
                        } catch (const std::exception& e) {
                            CancelQueue(launch_state);
                            ShowError("Stream startup failed", e.what());
                        }
                    };
                    if (launch_state->activity)
                        launch_state->activity->Dismiss(handoff);
                    else
                        handoff();
                } catch (const std::exception& e) {
                    CancelQueue(launch_state);
                    ShowError("Stream startup failed", e.what());
                }
            });

        } catch (const std::exception& e) {
            const std::string failed_session = launch_state->launch.TakeForCleanup();
            if (!failed_session.empty()) {
                try { bg_client.StopSession(bg_auth, failed_session); } catch (...) {}
            }
            const session_error::Presentation error = session_error::Present(e.what());
            brls::sync([=]() {
                if (!launch_state->launch.running()) return;
                CancelQueue(launch_state);
                launch_state->title = error.title;
                launch_state->detail = error.body + "\n\n" + Tr("Press B to return to the game page.");
                if (launch_state->view)
                    UpdateQueue(launch_state);
                else
                    brls::Application::notify(Tr(error.title) + ": " + Tr(error.body));
            });
        }
    });
}

}

void LaunchSessionDialog(const GfnClient& client, const AuthSession& auth,
                         const std::string& launch_app_id, const std::string& title,
                         const std::string& launch_store,
                         const std::string& internal_title,
                         const std::string& history_game_id,
                         const std::string& image_url)
{
    const auto connection = NetworkUtils::GetConnectionInfo();
    if (connection.connected && connection.type == NetworkConnectionType::Wifi &&
        network::ShouldWarnForStreaming(connection.wifi_band))
    {
        auto* dialog = new brls::Dialog(
            Tr("You're using 2.4 GHz. Please use 5 GHz network."));
        dialog->addButton(Tr("Cancel"), [] {});
        dialog->addButton(Tr("Continue anyway"), [=] {
            BeginLaunchSessionDialog(client, auth, launch_app_id, title,
                                     launch_store, internal_title,
                                     history_game_id, image_url);
        });
        dialog->setCancelable(true);
        dialog->open();
        return;
    }

    BeginLaunchSessionDialog(client, auth, launch_app_id, title, launch_store,
                             internal_title, history_game_id, image_url);
}

} // namespace opennow
