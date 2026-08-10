#pragma once
#include <borealis.hpp>
#include "controller_assignment_policy.hpp"
#include "gfn_client.hpp"
#include "controller_delivery_policy.hpp"
#include "keyboard_input_policy.hpp"
#include "network_utils.hpp"
#include "nte_credentials.hpp"
#include "stream_end_policy.hpp"
#include "stream_overlay_policy.hpp"
#include "webrtc_session.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

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
    void UpdatePerformanceCounter();
    void DrawPerformanceOverlay(NVGcontext* vg, float x, float y);
    void SetStreamOverlayVisible(bool visible);
    void DrawStreamOverlay(NVGcontext* vg, float x, float y, float width, float height,
                           std::chrono::steady_clock::time_point now);
    void RefreshNetworkInfo(std::chrono::steady_clock::time_point now);
    void DrawNetworkWarning(NVGcontext* vg, float x, float y, float width, float height,
                            std::chrono::steady_clock::time_point now);
    void UpdateSessionLimitNotice(std::chrono::steady_clock::time_point now);
    void DrawSessionLimitNotice(NVGcontext* vg, float x, float y, float width,
                                std::chrono::steady_clock::time_point now);
    void UpdateStreamEndState(std::chrono::steady_clock::time_point now);
    void BeginStreamEnd(opennow::StreamEndReason reason,
                        std::chrono::steady_clock::time_point now);
    void DrawStreamEndNotice(NVGcontext* vg, float x, float y, float width,
                             std::chrono::steady_clock::time_point now);
    void PollControllerStates(std::chrono::steady_clock::time_point now);
    void SendControllerInputs(std::chrono::steady_clock::time_point now);
    void ResetControllerDeliveryState();
    void SendNeutralControllerReports();
    void QueueControllerConnectedNotice(
        std::size_t controller, std::chrono::steady_clock::time_point now);
    void UpdateControllerNotice(std::chrono::steady_clock::time_point now);
    void DrawControllerNotice(NVGcontext* vg, float x, float y, float width,
                              std::chrono::steady_clock::time_point now);
#ifdef __SWITCH__
    bool ReadSwitchControllerSource(std::size_t source, brls::ControllerState& state);
#endif
    void OpenInlineKeyboard();
    void HideInlineKeyboard(bool send_enter);
    void UpdateInlineKeyboard();
    void HandleKeyboardText(const char* text);
    void SendKeyboardCharacter(char character);
    void SendKeyboardShortcut(opennow::input::KeyboardShortcut shortcut);
    void StartNteAutoLogin(std::chrono::steady_clock::time_point now);
    void CancelNteAutoLogin();
    void UpdateNteAutoLogin(std::chrono::steady_clock::time_point now);
    void SendNteClick(float normalized_x, float normalized_y);
    void ReanchorRemotePointer(float target_x, float target_y);
    static int16_t ClampMouseDelta(float value);
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

    struct ControllerDeliveryState {
        bool initialized = false;
        bool pending_disconnect = false;
        bool plus_was_down = false;
        bool plus_long_press = false;
        uint16_t last_buttons = 0;
        uint8_t last_left_trigger = 0;
        uint8_t last_right_trigger = 0;
        int16_t last_lx = 0;
        int16_t last_ly = 0;
        int16_t last_rx = 0;
        int16_t last_ry = 0;
        std::chrono::steady_clock::time_point plus_pressed_at {};
        std::chrono::steady_clock::time_point last_report {};
        opennow::input::StartDeliveryPulse start_pulse;
    };

    std::unique_ptr<WebRtcSession> session_;
    opennow::GfnClient client_;
    opennow::AuthSession auth_;
    std::string cloud_session_id_;
    std::string game_title_;
    bool cloud_stop_requested_ = false;
    bool debug_diagnostics_ = false;
    bool stats_overlay_enabled_ = false;
    bool stream_overlay_visible_ = false;
    bool stream_overlay_b_was_down_ = false;
    bool overlay_chord_latched_ = false;
    opennow::input::OverlayChordState overlay_chord_state_;
    std::string stream_codec_ = "H264";
    std::string stream_region_ = "Auto";
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
    bool keyboard_escape_chord_was_down_ = false;
    bool keyboard_tab_chord_was_down_ = false;
    bool keyboard_alt_tab_chord_was_down_ = false;
    bool keyboard_windows_chord_was_down_ = false;
    bool suppress_b_until_release_ = false;
    bool keyboard_release_guard_ = false;
    bool keyboard_visible_ = false;
    bool keyboard_launched_ = false;
    std::string keyboard_text_;
#ifdef __SWITCH__
    SwkbdInline inline_keyboard_ {};
#endif
    opennow::input::ControllerAssignments controller_assignments_;
    std::array<brls::ControllerState, opennow::input::kRemoteControllerCount>
        controller_states_ {};
    std::array<bool, opennow::input::kRemoteControllerCount> controller_connected_ {};
    std::array<ControllerDeliveryState, opennow::input::kRemoteControllerCount>
        controller_delivery_ {};
    bool controller_connections_initialized_ = false;
    std::string controller_notice_text_;
    std::deque<std::string> controller_notice_queue_;
    std::chrono::steady_clock::time_point controller_notice_visible_until_ {};
#ifdef __SWITCH__
    std::array<PadState, opennow::input::kSwitchControllerSourceCount>
        switch_controller_sources_ {};
#endif
    std::chrono::steady_clock::time_point fps_window_started_ {};
    VideoPerformanceCounters previous_video_counters_ {};
    float incoming_fps_ = 0.0f;
    float decoded_fps_ = 0.0f;
    float presented_fps_ = 0.0f;
    float stream_bitrate_mbps_ = 0.0f;
    int network_rtt_ms_ = -1;
    StreamNetworkCounters network_counters_ {};
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
    std::chrono::steady_clock::time_point last_network_info_check_at_ {};
    std::chrono::steady_clock::time_point network_warning_visible_until_ {};
    opennow::NetworkConnectionInfo network_info_ {};
    bool internet_connected_ = true;
};
