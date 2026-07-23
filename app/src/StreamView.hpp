#pragma once
#include <borealis.hpp>
#include "gfn_client.hpp"
#include "controller_delivery_policy.hpp"
#include "nte_credentials.hpp"
#include "webrtc_session.hpp"
#include <memory>
#include <vector>
#include <chrono>
#ifdef __SWITCH__
#include <switch.h>
#endif

class StreamView : public brls::Box {
public:
    StreamView(
        const std::string& signaling_url,
        const std::string& jwt_token,
        const std::string& session_id,
        const std::string& media_ip,
        int media_port,
        const std::vector<opennow::IceServerInfo>& ice_servers,
        const opennow::GfnClient& client,
        const opennow::AuthSession& auth,
        const std::string& game_title);
    ~StreamView() override;

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) override;
    void onFocusGained() override;
    void onFocusLost() override;

    static brls::View* create(
        const std::string& signaling_url,
        const std::string& jwt_token,
        const std::string& session_id,
        const std::string& media_ip,
        int media_port,
        const std::vector<opennow::IceServerInfo>& ice_servers,
        const opennow::GfnClient& client,
        const opennow::AuthSession& auth,
        const std::string& game_title);

private:
    void ExitStream();
    void StopCloudSessionAsync();
    void DrawDebugOverlay(NVGcontext* vg, float x, float y, float width);
    void DrawPreparingStream(NVGcontext* vg, float x, float y, float width, float height);
    void UpdatePerformanceCounter(const VideoPerformanceCounters& counters);
    void DrawPerformanceOverlay(NVGcontext* vg, float x, float y);
    void UpdateSessionLimitNotice(std::chrono::steady_clock::time_point now);
    void DrawSessionLimitNotice(NVGcontext* vg, float x, float y, float width,
                                std::chrono::steady_clock::time_point now);
    void UpdateStreamEndState(std::chrono::steady_clock::time_point now);
    void BeginStreamEnd(opennow::StreamEndReason reason,
                        std::chrono::steady_clock::time_point now);
    void DrawStreamEndNotice(NVGcontext* vg, float x, float y, float width,
                             std::chrono::steady_clock::time_point now);
    void OpenInlineKeyboard();
    void HideInlineKeyboard(bool send_enter);
    void UpdateInlineKeyboard();
    void HandleKeyboardText(const char* text);
    void SendKeyboardCharacter(char character);
    void StartNteAutoLogin(std::chrono::steady_clock::time_point now);
    void CancelNteAutoLogin();
    void UpdateNteAutoLogin(std::chrono::steady_clock::time_point now);
    void SendNteClick(float normalized_x, float normalized_y);
    void ReanchorRemotePointer(float target_x, float target_y);
    void ClearNteFocusedField();
    void SubmitNteFocusedField(const char* stage);
    void DrawNteAutoLoginStatus(NVGcontext* vg, float x, float y, float width, float height,
                                std::chrono::steady_clock::time_point now);
#ifdef __SWITCH__
    static void KeyboardChangedCallback(const char* text, SwkbdChangedStringArg* arg);
    static void KeyboardEnterCallback(const char* text, SwkbdDecidedEnterArg* arg);
    static void KeyboardCancelCallback();
    static StreamView* active_keyboard_view_;
#endif

    enum class SessionLimitNotice {
        None,
        FiveMinutes,
        OneMinute,
        Ended,
    };

    enum class NteAutoLoginStage {
        Idle,
        ClickEmailProvider,
        WaitEmailPage,
        FocusEmail,
        TypeEmail,
        SubmitEmail,
        WaitPasswordPage,
        FocusPassword,
        TypePassword,
        SubmitLogin,
    };

    std::unique_ptr<WebRtcSession> session_;
    opennow::GfnClient client_;
    opennow::AuthSession auth_;
    std::string cloud_session_id_;
    std::string game_title_;
    bool cloud_stop_requested_ = false;
    bool debug_diagnostics_ = false;
    bool stats_overlay_enabled_ = false;
    std::string controller_layout_ = "Xbox";
    bool show_debug_overlay_ = false;
    bool is_nte_session_ = false;
    bool nte_combo_was_down_ = false;
    bool nte_cancel_was_down_ = false;
    NteAutoLoginStage nte_stage_ = NteAutoLoginStage::Idle;
    opennow::NteCredentials nte_credentials_;
    std::string nte_text_buffer_;
    size_t nte_text_index_ = 0;
    std::string nte_status_;
    std::chrono::steady_clock::time_point nte_next_action_ {};
    std::chrono::steady_clock::time_point nte_status_until_ {};
    bool plus_was_down_ = false;
    bool plus_long_press_ = false;
    bool exit_requested_ = false;
    bool exit_combo_was_down_ = false;
    bool touch_was_down_ = false;
    bool touch_pointer_initialized_ = false;
    float touch_pointer_x_ = 0.0f;
    float touch_pointer_y_ = 0.0f;
    int touch_pointer_stream_width_ = 0;
    int touch_pointer_stream_height_ = 0;
    bool keyboard_combo_was_down_ = false;
    bool keyboard_b_was_down_ = false;
    bool keyboard_plus_was_down_ = false;
    bool suppress_b_until_release_ = false;
    bool keyboard_release_guard_ = false;
    bool keyboard_visible_ = false;
    bool keyboard_launched_ = false;
    std::string keyboard_text_;
#ifdef __SWITCH__
    SwkbdInline inline_keyboard_ {};
#endif
    std::chrono::steady_clock::time_point plus_pressed_at_ {};
    opennow::input::StartDeliveryPulse start_pulse_;
    std::chrono::steady_clock::time_point fps_window_started_ {};
    VideoPerformanceCounters previous_video_counters_ {};
    float incoming_fps_ = 0.0f;
    float decoded_fps_ = 0.0f;
    float presented_fps_ = 0.0f;
    float stream_bitrate_mbps_ = 0.0f;
    int network_rtt_ms_ = -1;
    bool gamepad_state_initialized_ = false;
    uint16_t last_gamepad_buttons_ = 0;
    uint8_t last_left_trigger_ = 0;
    uint8_t last_right_trigger_ = 0;
    int16_t last_lx_ = 0;
    int16_t last_ly_ = 0;
    int16_t last_rx_ = 0;
    int16_t last_ry_ = 0;
    std::chrono::steady_clock::time_point last_gamepad_report_ {};
    bool free_tier_session_ = false;
    bool five_minute_warning_shown_ = false;
    bool one_minute_warning_shown_ = false;
    bool session_limit_ended_ = false;
    SessionLimitNotice session_limit_notice_ = SessionLimitNotice::None;
    std::chrono::steady_clock::time_point stream_started_at_ {};
    std::chrono::steady_clock::time_point session_notice_visible_until_ {};
    opennow::StreamEndReason stream_end_reason_ = opennow::StreamEndReason::None;
    std::chrono::steady_clock::time_point stream_end_started_at_ {};
    std::chrono::steady_clock::time_point stream_auto_exit_at_ {};
    std::chrono::steady_clock::time_point last_network_check_at_ {};
    bool internet_connected_ = true;
};
