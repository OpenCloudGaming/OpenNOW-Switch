#include "StreamView.hpp"
#include "input/TouchMapping.hpp"
#include "stream/ffmpeg/AVFrameHolder.hpp"
#include "stream_diagnostics.hpp"
#include "stream_settings.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>

namespace
{

bool IsFreeTier(std::string tier)
{
    std::transform(tier.begin(), tier.end(), tier.begin(), [](unsigned char value) {
        return static_cast<char>(std::toupper(value));
    });
    return tier == "FREE";
}

} // namespace

StreamView::StreamView(
    const std::string& signaling_url,
    const std::string& jwt_token,
    const std::string& session_id,
    const std::string& media_ip,
    int media_port,
    const std::vector<opennow::IceServerInfo>& ice_servers,
    const opennow::GfnClient& client,
    const opennow::AuthSession& auth,
    const std::string& game_title)
    : client_(client)
    , auth_(auth)
    , cloud_session_id_(session_id)
    , game_title_(game_title) {
    const auto stream_settings = opennow::LoadStreamSettings();
    debug_diagnostics_ = stream_settings.debug_diagnostics;
    stats_overlay_enabled_ = stream_settings.stats_overlay_enabled;
    stream_codec_ = stream_settings.codec;
    stream_region_ = stream_settings.region;
    controller_layout_ = stream_settings.controller_layout;
    opennow::SetStreamDiagnosticsEnabled(debug_diagnostics_);
    free_tier_session_ = auth_.user.membership_tier_verified &&
        IsFreeTier(auth_.user.membership_tier);
    stream_started_at_ = std::chrono::steady_clock::now();
    is_nte_session_ = opennow::IsNevernessToEverness(game_title_);
    if (is_nte_session_) {
        nte_credentials_ = opennow::LoadNteCredentials();
        nte_status_until_ = stream_started_at_ + std::chrono::seconds(12);
    }
#ifdef __SWITCH__
    padInitialize(&switch_controller_sources_[0], HidNpadIdType_Handheld);
    for (std::size_t source = 1; source < switch_controller_sources_.size(); ++source)
    {
        padInitialize(
            &switch_controller_sources_[source],
            static_cast<HidNpadIdType>(source - 1));
    }
#endif
    session_ = std::make_unique<WebRtcSession>(
        signaling_url,
        jwt_token,
        session_id,
        media_ip,
        media_port,
        ice_servers);
    session_->start();
    RefreshNetworkInfo(stream_started_at_);
    
    // Suggest the view to take all available space
    this->setWidth(brls::Application::windowWidth);
    this->setHeight(brls::Application::windowHeight);
    this->setHideHighlight(true);
    this->setHideHighlightBackground(true);
    this->setHideHighlightBorder(true);
    
    this->setFocusable(true);
}

StreamView::~StreamView() {
    opennow::SetSensitiveInputLoggingSuppressed(false);
    std::fill(nte_credentials_.password.begin(), nte_credentials_.password.end(), '\0');
    std::fill(nte_text_buffer_.begin(), nte_text_buffer_.end(), '\0');
#ifdef __SWITCH__
    if (active_keyboard_view_ == this)
        active_keyboard_view_ = nullptr;
    if (keyboard_launched_) {
        swkbdInlineClose(&inline_keyboard_);
        keyboard_launched_ = false;
    }
#endif
    brls::Application::getPlatform()->disableScreenDimming(false);
    brls::Application::getPlatform()->getInputManager()->setPointerLock(false);
    StopCloudSessionAsync();
    if (session_) {
        session_->stop();
    }
}

void StreamView::onFocusGained() {
    brls::Box::onFocusGained();
    brls::Application::getPlatform()->disableScreenDimming(true);
    brls::Application::getPlatform()->getInputManager()->setPointerLock(true);
}

void StreamView::onFocusLost() {
    brls::Box::onFocusLost();
    brls::Application::getPlatform()->disableScreenDimming(false);
    brls::Application::getPlatform()->getInputManager()->setPointerLock(false);
}

void StreamView::ExitStream() {
    if (exit_requested_)
        return;
    exit_requested_ = true;
    if (session_)
        session_->request_stop();
    StopCloudSessionAsync();
    if (!brls::Application::popActivity(brls::TransitionAnimation::FADE, [] {
            const auto activities = brls::Application::getActivitiesStack();
            if (!activities.empty() && activities.back() &&
                activities.back()->getContentView()) {
                brls::Application::giveFocus(activities.back()->getContentView());
            }
        })) {
        exit_requested_ = false;
    }
}

void StreamView::StopCloudSessionAsync() {
    if (cloud_stop_requested_ || cloud_session_id_.empty())
        return;

    cloud_stop_requested_ = true;
    opennow::GfnClient bg_client = client_;
    opennow::AuthSession bg_auth = auth_;
    std::string bg_session_id = cloud_session_id_;
    brls::async([bg_client, bg_auth, bg_session_id]() mutable {
        try {
            bg_client.StopSession(bg_auth, bg_session_id);
        } catch (...) {
        }
    });
}

void StreamView::draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) {
    if (session_) {
        session_->poll();
        UpdateInlineKeyboard();

        const auto stream_now = std::chrono::steady_clock::now();
        UpdateSessionLimitNotice(stream_now);
        UpdateStreamEndState(stream_now);
        if (stream_end_reason_ != opennow::StreamEndReason::None &&
            stream_now >= stream_auto_exit_at_) {
            ExitStream();
            return;
        }
        
        brls::ControllerState state;
        brls::Application::getPlatform()->getInputManager()->updateUnifiedControllerState(&state);

        const auto input_now = std::chrono::steady_clock::now();
        PollControllerStates(input_now);
        bool nte_owned_input = false;
        if (is_nte_session_ && !stream_overlay_visible_ &&
            stream_end_reason_ == opennow::StreamEndReason::None) {
            const bool nte_combo = state.buttons[brls::BUTTON_LB] &&
                                   state.buttons[brls::BUTTON_X];
            if (nte_combo && !nte_combo_was_down_)
                StartNteAutoLogin(input_now);
            nte_combo_was_down_ = nte_combo;

            const bool was_active = nte_stage_ != NteAutoLoginStage::Idle;
            const bool cancel_down = state.buttons[brls::BUTTON_B];
            if (was_active && cancel_down && !nte_cancel_was_down_)
                CancelNteAutoLogin();
            else if (was_active)
                UpdateNteAutoLogin(input_now);
            nte_cancel_was_down_ = cancel_down;
            nte_owned_input = was_active || nte_combo;
        }

        const bool minus_down = state.buttons[brls::BUTTON_BACK];
        const bool plus_down_for_overlay = state.buttons[brls::BUTTON_START];
        const auto chord_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            input_now.time_since_epoch()).count();
        const auto overlay_decision = opennow::input::EvaluateOverlayChord(
            minus_down, plus_down_for_overlay, overlay_chord_state_, chord_now_ms);
        overlay_chord_state_ = overlay_decision.next_state;
        if (!minus_down && !plus_down_for_overlay)
            overlay_chord_latched_ = false;
        if (overlay_decision.toggle_overlay && !overlay_chord_latched_ &&
            !keyboard_visible_ && !nte_owned_input &&
            stream_end_reason_ == opennow::StreamEndReason::None)
        {
            overlay_chord_latched_ = true;
            SetStreamOverlayVisible(!stream_overlay_visible_);
        }

        if (stream_overlay_visible_)
        {
            const bool b_down = state.buttons[brls::BUTTON_B];
            if (b_down && !stream_overlay_b_was_down_)
            {
                suppress_b_until_release_ = true;
                SetStreamOverlayVisible(false);
            }
            stream_overlay_b_was_down_ = b_down;
        }
        else
        {
            stream_overlay_b_was_down_ = false;
        }

        const bool keyboard_combo = minus_down && state.buttons[brls::BUTTON_Y];
        bool keyboard_owned_input = keyboard_visible_ || stream_overlay_visible_ ||
            overlay_decision.preempt_input || overlay_chord_latched_ ||
            nte_owned_input || stream_end_reason_ != opennow::StreamEndReason::None;
        if (keyboard_combo && !keyboard_combo_was_down_ && !nte_owned_input &&
            !stream_overlay_visible_ &&
            stream_end_reason_ == opennow::StreamEndReason::None) {
            OpenInlineKeyboard();
            keyboard_owned_input = keyboard_visible_;
        }
        keyboard_combo_was_down_ = keyboard_combo;

        if (keyboard_visible_) {
            const bool b_down = state.buttons[brls::BUTTON_B];
            const bool plus_down = state.buttons[brls::BUTTON_START];
            if (plus_down && !keyboard_plus_was_down_)
                HideInlineKeyboard(true);
            else if (b_down && !keyboard_b_was_down_)
                HideInlineKeyboard(false);
            keyboard_b_was_down_ = b_down;
            keyboard_plus_was_down_ = plus_down;
            keyboard_owned_input = true;
        } else {
            keyboard_b_was_down_ = false;
            keyboard_plus_was_down_ = false;
        }

        if (!state.buttons[brls::BUTTON_B])
            suppress_b_until_release_ = false;

        if (keyboard_release_guard_) {
            const bool controller_active =
                state.buttons[brls::BUTTON_A] || state.buttons[brls::BUTTON_B] ||
                state.buttons[brls::BUTTON_X] || state.buttons[brls::BUTTON_Y] ||
                state.buttons[brls::BUTTON_UP] || state.buttons[brls::BUTTON_DOWN] ||
                state.buttons[brls::BUTTON_LEFT] || state.buttons[brls::BUTTON_RIGHT] ||
                state.buttons[brls::BUTTON_LB] || state.buttons[brls::BUTTON_RB] ||
                state.buttons[brls::BUTTON_LT] || state.buttons[brls::BUTTON_RT] ||
                state.buttons[brls::BUTTON_START] || state.buttons[brls::BUTTON_BACK] ||
                std::fabs(state.axes[brls::LEFT_X]) > 0.15f ||
                std::fabs(state.axes[brls::LEFT_Y]) > 0.15f ||
                std::fabs(state.axes[brls::RIGHT_X]) > 0.15f ||
                std::fabs(state.axes[brls::RIGHT_Y]) > 0.15f;
            if (controller_active)
                keyboard_owned_input = true;
            else
                keyboard_release_guard_ = false;
        }

        if (!keyboard_owned_input) {
        
        const auto now = std::chrono::steady_clock::now();

        // Preserve the requested Minus -> View mapping. Exit the stream with
        // a deliberate ZL + ZR + Minus combination instead of stealing B.
        const bool exit_combo = state.buttons[brls::BUTTON_LT] &&
                               state.buttons[brls::BUTTON_RT] &&
                               state.buttons[brls::BUTTON_BACK];
        if (exit_combo && !exit_combo_was_down_) {
            exit_combo_was_down_ = true;
            ExitStream();
            return;
        }
        exit_combo_was_down_ = exit_combo;

        SendControllerInputs(now);

        std::vector<brls::RawTouchState> touches;
        brls::Application::getPlatform()->getInputManager()->updateTouchStates(&touches);
        bool touch_down = false;
        brls::Point touch_position {};
        for (const auto& touch : touches) {
            if (touch.pressed) {
                touch_down = true;
                touch_position = touch.position;
                break;
            }
        }

        if (touch_down) {
            const int stream_width = std::max(1, session_->stream_width());
            const int stream_height = std::max(1, session_->stream_height());
            const auto target = opennow::input::MapTouchToStream(
                touch_position.x, touch_position.y, x, y, width, height,
                stream_width, stream_height);

            if (!touch_pointer_initialized_ ||
                touch_pointer_stream_width_ != stream_width ||
                touch_pointer_stream_height_ != stream_height) {
                touch_pointer_x_ = stream_width * 0.5f;
                touch_pointer_y_ = stream_height * 0.5f;
                touch_pointer_stream_width_ = stream_width;
                touch_pointer_stream_height_ = stream_height;
                touch_pointer_initialized_ = true;
            }
            if (!touch_was_down_) {
                session_->record_ui_event(
                    "touch begin raw=" + std::to_string(touch_position.x) + "/" +
                    std::to_string(touch_position.y) + " target=" +
                    std::to_string(target.x) + "/" + std::to_string(target.y) +
                    " pointer=" + std::to_string(touch_pointer_x_) + "/" +
                    std::to_string(touch_pointer_y_) +
                    " frame=" + std::to_string(stream_width) + "x" +
                    std::to_string(stream_height) + " view=" +
                    std::to_string(width) + "x" + std::to_string(height));
                // GFN mouse input is relative and the remote cursor may have
                // moved independently. Re-anchor every new touch so the click
                // lands exactly where the finger is, then track drags locally.
                ReanchorRemotePointer(target.x, target.y);
                session_->send_mouse_left_button(true);
            } else {
                const int16_t dx = ClampMouseDelta(target.x - touch_pointer_x_);
                const int16_t dy = ClampMouseDelta(target.y - touch_pointer_y_);
                if (dx != 0 || dy != 0) {
                    session_->send_mouse_move(dx, dy);
                    touch_pointer_x_ = std::clamp(touch_pointer_x_ + dx, 0.0f,
                                                  static_cast<float>(stream_width - 1));
                    touch_pointer_y_ = std::clamp(touch_pointer_y_ + dy, 0.0f,
                                                  static_cast<float>(stream_height - 1));
                }
            }
        } else if (touch_was_down_) {
            session_->send_mouse_left_button(false);
        }
        touch_was_down_ = touch_down;
        }
    }
    
    // Basic drawing logic - if we have a frame, we draw it using GLVideoRenderer
    bool got_frame = false;
    const int64_t video_target = session_ ? session_->video_target_rtp_timestamp() : AV_NOPTS_VALUE;
    AVFrameHolder::instance().get([this, vg, width, height, &got_frame](AVFrame* frame, uint64_t generation, bool reused) {
        if (session_) {
            session_->draw(vg, width, height, frame, generation);
            got_frame = true;
        }
    }, video_target);
    if (session_)
        UpdatePerformanceCounter(session_->get_video_performance());
    
    if (!got_frame) {
        if (debug_diagnostics_) {
            nvgFillColor(vg, nvgRGB(0, 0, 0));
            nvgBeginPath(vg);
            nvgRect(vg, x, y, width, height);
            nvgFill(vg);
            DrawDebugOverlay(vg, x, y, width);
        } else {
            DrawPreparingStream(vg, x, y, width, height);
        }
    } else if (debug_diagnostics_ && show_debug_overlay_) {
        DrawDebugOverlay(vg, x, y, width);
    }

    const auto notice_now = std::chrono::steady_clock::now();
    if ((debug_diagnostics_ || stats_overlay_enabled_) &&
        stream_end_reason_ == opennow::StreamEndReason::None)
        DrawPerformanceOverlay(vg, x, y);
    if (stream_end_reason_ == opennow::StreamEndReason::None)
        DrawSessionLimitNotice(vg, x, y, width, notice_now);
    DrawStreamEndNotice(vg, x, y, width, notice_now);
    if (stream_end_reason_ == opennow::StreamEndReason::None)
        DrawNteAutoLoginStatus(vg, x, y, width, height, notice_now);
    if (stream_end_reason_ == opennow::StreamEndReason::None)
        DrawStreamOverlay(vg, x, y, width, height, notice_now);
    if (stream_end_reason_ == opennow::StreamEndReason::None)
        DrawNetworkWarning(vg, x, y, width, height, notice_now);
    if (stream_end_reason_ == opennow::StreamEndReason::None)
        DrawControllerNotice(vg, x, y, width, notice_now);

    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

brls::View* StreamView::create(
    const std::string& signaling_url,
    const std::string& jwt_token,
    const std::string& session_id,
    const std::string& media_ip,
    int media_port,
    const std::vector<opennow::IceServerInfo>& ice_servers,
    const opennow::GfnClient& client,
    const opennow::AuthSession& auth,
    const std::string& game_title) {
    return new StreamView(signaling_url, jwt_token, session_id, media_ip, media_port,
                          ice_servers, client, auth, game_title);
}
