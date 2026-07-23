#include "StreamView.hpp"
#include "controller_layout.hpp"
#include "input/TouchMapping.hpp"
#include "network_utils.hpp"
#include "nte_autologin_log.hpp"
#include "stream/ffmpeg/AVFrameHolder.hpp"
#include "stream_diagnostics.hpp"
#include "stream_settings.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{

void ApplyRadialDeadzone(float& x, float& y)
{
    constexpr float deadzone = 0.12f;
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= deadzone) {
        x = 0.0f;
        y = 0.0f;
        return;
    }

    const float normalized = std::min(1.0f, (magnitude - deadzone) / (1.0f - deadzone));
    const float scale = normalized / magnitude;
    x *= scale;
    y *= scale;
}

int16_t QuantizeAxis(float value)
{
    value = std::max(-1.0f, std::min(1.0f, value));
    return static_cast<int16_t>(std::lround(value * 32767.0f));
}

bool IsFreeTier(std::string tier)
{
    std::transform(tier.begin(), tier.end(), tier.begin(), [](unsigned char value) {
        return static_cast<char>(std::toupper(value));
    });
    return tier == "FREE";
}

struct KeyboardStroke {
    uint16_t keycode = 0;
    uint16_t scancode = 0;
    uint16_t modifiers = 0;
};

bool MapAsciiKey(char character, KeyboardStroke& stroke)
{
    constexpr uint16_t kShift = 0x0001;
    static constexpr uint8_t letter_scans[26] = {
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
    };
    if (character >= 'a' && character <= 'z') {
        const int index = character - 'a';
        stroke = {static_cast<uint16_t>('A' + index), letter_scans[index], 0};
        return true;
    }
    if (character >= 'A' && character <= 'Z') {
        const int index = character - 'A';
        stroke = {static_cast<uint16_t>('A' + index), letter_scans[index], kShift};
        return true;
    }
    if (character >= '1' && character <= '9') {
        stroke = {static_cast<uint16_t>(character), static_cast<uint16_t>(0x02 + character - '1'), 0};
        return true;
    }
    if (character == '0') {
        stroke = {'0', 0x0b, 0};
        return true;
    }

    switch (character) {
        case ' ': stroke = {0x20, 0x39, 0}; return true;
        case '@': stroke = {'2', 0x03, kShift}; return true;
        case '.': stroke = {0xbe, 0x34, 0}; return true;
        case ',': stroke = {0xbc, 0x33, 0}; return true;
        case '-': stroke = {0xbd, 0x0c, 0}; return true;
        case '_': stroke = {0xbd, 0x0c, kShift}; return true;
        case '=': stroke = {0xbb, 0x0d, 0}; return true;
        case '+': stroke = {0xbb, 0x0d, kShift}; return true;
        case '/': stroke = {0xbf, 0x35, 0}; return true;
        case '?': stroke = {0xbf, 0x35, kShift}; return true;
        case '\\': stroke = {0xdc, 0x2b, 0}; return true;
        case '|': stroke = {0xdc, 0x2b, kShift}; return true;
        case '[': stroke = {0xdb, 0x1a, 0}; return true;
        case '{': stroke = {0xdb, 0x1a, kShift}; return true;
        case ']': stroke = {0xdd, 0x1b, 0}; return true;
        case '}': stroke = {0xdd, 0x1b, kShift}; return true;
        case ':': stroke = {0xba, 0x27, kShift}; return true;
        case ';': stroke = {0xba, 0x27, 0}; return true;
        case '\'': stroke = {0xde, 0x28, 0}; return true;
        case '"': stroke = {0xde, 0x28, kShift}; return true;
        case '`': stroke = {0xc0, 0x29, 0}; return true;
        case '~': stroke = {0xc0, 0x29, kShift}; return true;
        case '<': stroke = {0xbc, 0x33, kShift}; return true;
        case '>': stroke = {0xbe, 0x34, kShift}; return true;
        case '!': stroke = {'1', 0x02, kShift}; return true;
        case '#': stroke = {'3', 0x04, kShift}; return true;
        case '$': stroke = {'4', 0x05, kShift}; return true;
        case '%': stroke = {'5', 0x06, kShift}; return true;
        case '^': stroke = {'6', 0x07, kShift}; return true;
        case '&': stroke = {'7', 0x08, kShift}; return true;
        case '*': stroke = {'8', 0x09, kShift}; return true;
        case '(': stroke = {'9', 0x0a, kShift}; return true;
        case ')': stroke = {'0', 0x0b, kShift}; return true;
        default: return false;
    }
}

int16_t ClampMouseDelta(float value)
{
    return static_cast<int16_t>(std::clamp<long>(std::lround(value), -32768L, 32767L));
}

} // namespace

#ifdef __SWITCH__
StreamView* StreamView::active_keyboard_view_ = nullptr;
#endif

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
    session_ = std::make_unique<WebRtcSession>(
        signaling_url,
        jwt_token,
        session_id,
        media_ip,
        media_port,
        ice_servers);
    session_->start();
    
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

void StreamView::SendKeyboardCharacter(char character) {
    if (!session_)
        return;

    KeyboardStroke stroke {};
    if (character == '\b')
        stroke = {0x08, 0x0e, 0};
    else if (character == '\n' || character == '\r')
        stroke = {0x0d, 0x1c, 0};
    else if (!MapAsciiKey(character, stroke)) {
        session_->record_ui_event("keyboard unsupported UTF-8 byte=" +
                                  std::to_string(static_cast<unsigned char>(character)));
        return;
    }

    session_->send_keyboard_key(stroke.keycode, stroke.scancode, stroke.modifiers, true);
    session_->send_keyboard_key(stroke.keycode, stroke.scancode, stroke.modifiers, false);
}

void StreamView::SendNteClick(float normalized_x, float normalized_y) {
    if (!session_) {
        opennow::AppendNteAutoLoginLog("CLICK skipped reason=no_session");
        return;
    }

    const int stream_width = std::max(1, session_->stream_width());
    const int stream_height = std::max(1, session_->stream_height());
    if (!touch_pointer_initialized_ ||
        touch_pointer_stream_width_ != stream_width ||
        touch_pointer_stream_height_ != stream_height) {
        touch_pointer_x_ = stream_width * 0.5f;
        touch_pointer_y_ = stream_height * 0.5f;
        touch_pointer_stream_width_ = stream_width;
        touch_pointer_stream_height_ = stream_height;
        touch_pointer_initialized_ = true;
    }

    const float target_x = std::clamp(normalized_x, 0.0f, 1.0f) * (stream_width - 1);
    const float target_y = std::clamp(normalized_y, 0.0f, 1.0f) * (stream_height - 1);
    opennow::AppendNteAutoLoginLog(
        "CLICK normalized=" + std::to_string(normalized_x) + "/" +
        std::to_string(normalized_y) + " target=" + std::to_string(target_x) + "/" +
        std::to_string(target_y) + " anchor=top_left stream=" +
        std::to_string(stream_width) + "x" +
        std::to_string(stream_height));
    ReanchorRemotePointer(target_x, target_y);
    session_->send_mouse_left_button(true);
    session_->send_mouse_left_button(false);
}

void StreamView::ReanchorRemotePointer(float target_x, float target_y) {
    if (!session_)
        return;

    const int stream_width = std::max(1, session_->stream_width());
    const int stream_height = std::max(1, session_->stream_height());
    target_x = std::clamp(target_x, 0.0f, static_cast<float>(stream_width - 1));
    target_y = std::clamp(target_y, 0.0f, static_cast<float>(stream_height - 1));

    // GFN exposes relative mouse movement. Two oversized moves guarantee that
    // the remote cursor reaches the top-left edge regardless of its real
    // position, eliminating drift between our model and the streamed cursor.
    session_->send_mouse_move(-4096, -4096);
    session_->send_mouse_move(-4096, -4096);
    session_->send_mouse_move(ClampMouseDelta(target_x), ClampMouseDelta(target_y));
    touch_pointer_x_ = target_x;
    touch_pointer_y_ = target_y;
    touch_pointer_stream_width_ = stream_width;
    touch_pointer_stream_height_ = stream_height;
    touch_pointer_initialized_ = true;
}

void StreamView::ClearNteFocusedField() {
    if (!session_)
        return;
    // Modifier bit 1 is Ctrl in the NVST keyboard protocol. Clearing the
    // active field makes retries idempotent instead of appending credentials.
    constexpr uint16_t kControl = 0x0002;
    session_->send_keyboard_key('A', 0x1e, kControl, true);
    session_->send_keyboard_key('A', 0x1e, kControl, false);
    session_->send_keyboard_key(0x08, 0x0e, 0, true);
    session_->send_keyboard_key(0x08, 0x0e, 0, false);
    opennow::AppendNteAutoLoginLog("FIELD clear select_all_backspace content=redacted");
}

void StreamView::SubmitNteFocusedField(const char* stage) {
    SendKeyboardCharacter('\n');
    opennow::AppendNteAutoLoginLog(
        "FORM submit method=enter stage=" + std::string(stage ? stage : "unknown"));
}

void StreamView::StartNteAutoLogin(std::chrono::steady_clock::time_point now) {
    if (!is_nte_session_ || nte_stage_ != NteAutoLoginStage::Idle)
        return;
    if (keyboard_visible_) {
        nte_status_ = "Close the on-screen keyboard before NTE Auto-login";
        nte_status_until_ = now + std::chrono::seconds(4);
        return;
    }

    nte_credentials_ = opennow::LoadNteCredentials();
    if (!nte_credentials_.valid()) {
        opennow::ResetNteAutoLoginLog("Neverness to Everness");
        opennow::AppendNteAutoLoginLog("START rejected reason=credentials_missing_or_invalid");
        nte_status_ = "NTE Auto-login is not configured on the game page";
        nte_status_until_ = now + std::chrono::seconds(5);
        brls::Application::notify("Configure NTE Auto-login on the Neverness to Everness game page");
        return;
    }

    nte_stage_ = NteAutoLoginStage::ClickEmailProvider;
    opennow::SetSensitiveInputLoggingSuppressed(true);
    opennow::ResetNteAutoLoginLog("Neverness to Everness");
    opennow::AppendNteAutoLoginLog(
        "FLOW revision=0.9.4.5 submit=email_enter,password_enter field_clear=ctrl_a_backspace");
    opennow::AppendNteAutoLoginLog("START accepted credentials=present_and_redacted");
    if (session_)
        opennow::AppendNteAutoLoginLog("TRANSPORT snapshot\n" + session_->get_debug_info());
    nte_next_action_ = now;
    nte_status_ = "NTE Auto-login: opening email sign-in";
    nte_status_until_ = now + std::chrono::seconds(20);
    if (session_)
        session_->record_ui_event("nte_autologin started");
}

void StreamView::CancelNteAutoLogin() {
    if (nte_stage_ == NteAutoLoginStage::Idle)
        return;
    std::fill(nte_text_buffer_.begin(), nte_text_buffer_.end(), '\0');
    nte_text_buffer_.clear();
    nte_text_index_ = 0;
    nte_stage_ = NteAutoLoginStage::Idle;
    opennow::SetSensitiveInputLoggingSuppressed(false);
    opennow::AppendNteAutoLoginLog("CANCEL source=user_button_b");
    nte_status_ = "NTE Auto-login cancelled";
    nte_status_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    if (session_)
        session_->record_ui_event("nte_autologin cancelled");
}

void StreamView::UpdateNteAutoLogin(std::chrono::steady_clock::time_point now) {
    if (nte_stage_ == NteAutoLoginStage::Idle || now < nte_next_action_)
        return;

    switch (nte_stage_) {
        case NteAutoLoginStage::ClickEmailProvider:
            opennow::AppendNteAutoLoginLog("STAGE click_email_provider");
            SendNteClick(0.59f, 0.43f);
            nte_stage_ = NteAutoLoginStage::WaitEmailPage;
            nte_next_action_ = now + std::chrono::milliseconds(2500);
            nte_status_ = "NTE Auto-login: waiting for email form";
            break;
        case NteAutoLoginStage::WaitEmailPage:
            opennow::AppendNteAutoLoginLog("STAGE email_page_wait_complete delay_ms=2500");
            nte_stage_ = NteAutoLoginStage::FocusEmail;
            nte_next_action_ = now;
            break;
        case NteAutoLoginStage::FocusEmail:
            opennow::AppendNteAutoLoginLog("STAGE focus_email_field");
            SendNteClick(0.55f, 0.35f);
            ClearNteFocusedField();
            nte_text_buffer_ = nte_credentials_.email;
            nte_text_index_ = 0;
            nte_stage_ = NteAutoLoginStage::TypeEmail;
            nte_next_action_ = now + std::chrono::milliseconds(220);
            nte_status_ = "NTE Auto-login: entering email";
            opennow::AppendNteAutoLoginLog("TYPE email begin content=redacted");
            break;
        case NteAutoLoginStage::TypeEmail:
            if (nte_text_index_ < nte_text_buffer_.size()) {
                SendKeyboardCharacter(nte_text_buffer_[nte_text_index_++]);
                nte_next_action_ = now + std::chrono::milliseconds(35);
            } else {
                nte_text_buffer_.clear();
                opennow::AppendNteAutoLoginLog("TYPE email complete content=redacted");
                nte_stage_ = NteAutoLoginStage::SubmitEmail;
                nte_next_action_ = now + std::chrono::milliseconds(300);
            }
            break;
        case NteAutoLoginStage::SubmitEmail:
            opennow::AppendNteAutoLoginLog("STAGE submit_email method=enter");
            SubmitNteFocusedField("email");
            nte_stage_ = NteAutoLoginStage::WaitPasswordPage;
            nte_next_action_ = now + std::chrono::milliseconds(5500);
            nte_status_ = "NTE Auto-login: waiting for password form";
            opennow::AppendNteAutoLoginLog(
                "GUARD password_input_deferred_until_page_transition delay_ms=5500");
            break;
        case NteAutoLoginStage::WaitPasswordPage:
            opennow::AppendNteAutoLoginLog("STAGE password_page_wait_complete delay_ms=5500");
            nte_stage_ = NteAutoLoginStage::FocusPassword;
            nte_next_action_ = now;
            break;
        case NteAutoLoginStage::FocusPassword:
            opennow::AppendNteAutoLoginLog("STAGE focus_password_field");
            SendNteClick(0.55f, 0.45f);
            ClearNteFocusedField();
            nte_text_buffer_ = nte_credentials_.password;
            nte_text_index_ = 0;
            nte_stage_ = NteAutoLoginStage::TypePassword;
            nte_next_action_ = now + std::chrono::milliseconds(220);
            nte_status_ = "NTE Auto-login: entering password";
            opennow::AppendNteAutoLoginLog("TYPE password begin content=redacted");
            break;
        case NteAutoLoginStage::TypePassword:
            if (nte_text_index_ < nte_text_buffer_.size()) {
                SendKeyboardCharacter(nte_text_buffer_[nte_text_index_++]);
                nte_next_action_ = now + std::chrono::milliseconds(35);
            } else {
                std::fill(nte_text_buffer_.begin(), nte_text_buffer_.end(), '\0');
                nte_text_buffer_.clear();
                opennow::AppendNteAutoLoginLog("TYPE password complete content=redacted");
                nte_stage_ = NteAutoLoginStage::SubmitLogin;
                nte_next_action_ = now + std::chrono::milliseconds(300);
            }
            break;
        case NteAutoLoginStage::SubmitLogin:
            opennow::AppendNteAutoLoginLog("STAGE submit_login method=enter");
            SubmitNteFocusedField("password");
            nte_stage_ = NteAutoLoginStage::Idle;
            opennow::SetSensitiveInputLoggingSuppressed(false);
            nte_status_ = "NTE Auto-login: sign-in submitted";
            nte_status_until_ = now + std::chrono::seconds(5);
            if (session_)
                session_->record_ui_event("nte_autologin submitted");
            opennow::AppendNteAutoLoginLog("COMPLETE login_submitted");
            break;
        case NteAutoLoginStage::Idle:
            break;
    }
}

void StreamView::HandleKeyboardText(const char* text) {
    const std::string updated = text ? text : "";
    size_t prefix = 0;
    while (prefix < keyboard_text_.size() && prefix < updated.size() &&
           keyboard_text_[prefix] == updated[prefix]) {
        prefix++;
    }

    for (size_t i = prefix; i < keyboard_text_.size(); ++i)
        SendKeyboardCharacter('\b');
    for (size_t i = prefix; i < updated.size(); ++i)
        SendKeyboardCharacter(updated[i]);
    keyboard_text_ = updated;
}

void StreamView::OpenInlineKeyboard() {
#ifdef __SWITCH__
    if (keyboard_visible_)
        return;

    if (!keyboard_launched_) {
        Result rc = swkbdInlineCreate(&inline_keyboard_);
        if (R_FAILED(rc)) {
            if (session_)
                session_->record_ui_event("keyboard create failed rc=" + std::to_string(rc));
            return;
        }

        rc = swkbdInlineLaunchForLibraryApplet(
            &inline_keyboard_, static_cast<u8>(SwkbdInlineMode_AppletDisplay), 1);
        if (R_FAILED(rc)) {
            swkbdInlineClose(&inline_keyboard_);
            if (session_)
                session_->record_ui_event("keyboard launch failed rc=" + std::to_string(rc));
            return;
        }

        keyboard_launched_ = true;
        active_keyboard_view_ = this;
        swkbdInlineSetChangedStringCallback(&inline_keyboard_, KeyboardChangedCallback);
        swkbdInlineSetDecidedEnterCallback(&inline_keyboard_, KeyboardEnterCallback);
        swkbdInlineSetDecidedCancelCallback(&inline_keyboard_, KeyboardCancelCallback);
        swkbdInlineSetAlphaEnabledInInputMode(&inline_keyboard_, true);
        swkbdInlineSetKeytopBgAlpha(&inline_keyboard_, 0.72f);
        swkbdInlineSetFooterBgAlpha(&inline_keyboard_, 0.62f);
        swkbdInlineSetKeytopAsFloating(&inline_keyboard_, true);
        swkbdInlineSetFooterScalable(&inline_keyboard_, true);
        swkbdInlineSetTouchFlag(&inline_keyboard_, true);
        swkbdInlineSetDirectionalButtonAssignFlag(&inline_keyboard_, true);
    }

    keyboard_text_.clear();
    swkbdInlineSetInputText(&inline_keyboard_, "");
    swkbdInlineSetCursorPos(&inline_keyboard_, 0);
    SwkbdAppearArg appear {};
    swkbdInlineMakeAppearArg(&appear, SwkbdType_Latin);
    swkbdInlineAppearArgSetStringLenMax(&appear, 500);
    swkbdInlineAppearArgSetOkButtonText(&appear, "Enter");
    swkbdInlineAppear(&inline_keyboard_, &appear);
    keyboard_visible_ = true;
    keyboard_b_was_down_ = false;
    keyboard_plus_was_down_ = false;
    gamepad_state_initialized_ = false;
    if (touch_was_down_) {
        session_->send_mouse_left_button(false);
        touch_was_down_ = false;
    }
    session_->send_gamepad_input(0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f);
    session_->record_ui_event("keyboard opened by Minus+Y");
#endif
}

void StreamView::HideInlineKeyboard(bool send_enter) {
#ifdef __SWITCH__
    if (!keyboard_visible_)
        return;
    if (send_enter)
        SendKeyboardCharacter('\n');
    swkbdInlineDisappear(&inline_keyboard_);
    keyboard_visible_ = false;
    keyboard_text_.clear();
    gamepad_state_initialized_ = false;
    suppress_b_until_release_ = true;
    keyboard_release_guard_ = true;
    if (session_)
        session_->record_ui_event(send_enter ? "keyboard submitted" : "keyboard hidden by B");
#else
    (void)send_enter;
#endif
}

void StreamView::UpdateInlineKeyboard() {
#ifdef __SWITCH__
    if (!keyboard_launched_)
        return;
    SwkbdState state = SwkbdState_Inactive;
    const Result rc = swkbdInlineUpdate(&inline_keyboard_, &state);
    if (R_FAILED(rc) && session_)
        session_->record_ui_event("keyboard update failed rc=" + std::to_string(rc));
#endif
}

#ifdef __SWITCH__
void StreamView::KeyboardChangedCallback(const char* text, SwkbdChangedStringArg*) {
    if (active_keyboard_view_)
        active_keyboard_view_->HandleKeyboardText(text);
}

void StreamView::KeyboardEnterCallback(const char* text, SwkbdDecidedEnterArg*) {
    if (!active_keyboard_view_)
        return;
    active_keyboard_view_->HandleKeyboardText(text);
    active_keyboard_view_->SendKeyboardCharacter('\n');
    active_keyboard_view_->keyboard_visible_ = false;
    active_keyboard_view_->keyboard_text_.clear();
    active_keyboard_view_->gamepad_state_initialized_ = false;
    active_keyboard_view_->keyboard_release_guard_ = true;
    if (active_keyboard_view_->session_)
        active_keyboard_view_->session_->record_ui_event("keyboard submitted");
}

void StreamView::KeyboardCancelCallback() {
    if (!active_keyboard_view_)
        return;
    active_keyboard_view_->keyboard_visible_ = false;
    active_keyboard_view_->keyboard_text_.clear();
    active_keyboard_view_->gamepad_state_initialized_ = false;
    active_keyboard_view_->suppress_b_until_release_ = true;
    active_keyboard_view_->keyboard_release_guard_ = true;
    if (active_keyboard_view_->session_)
        active_keyboard_view_->session_->record_ui_event("keyboard cancelled");
}
#endif

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

void StreamView::DrawDebugOverlay(NVGcontext* vg, float x, float y, float width) {
    if (!session_)
        return;

    std::string debug_text = session_->get_debug_info();
    nvgFontSize(vg, 24.0f);
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgTextBox(vg, x + 50.0f, y + 50.0f, width - 100.0f, debug_text.c_str(), nullptr);
}

void StreamView::DrawPreparingStream(
    NVGcontext* vg, float x, float y, float width, float height) {
    constexpr float kPi = 3.14159265358979323846f;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    const StreamTransportHealth health = session_
        ? session_->get_transport_health()
        : StreamTransportHealth {};
    const VideoPerformanceCounters counters = session_
        ? session_->get_video_performance()
        : VideoPerformanceCounters {};

    int stage = 1;
    std::string title = "Connecting to your cloud rig...";
    std::string detail = "Opening the secure signaling channel";
    if (health.signaling_connected)
    {
        stage = 1;
        title = "Negotiating the stream...";
        detail = "Selecting the fastest network path";
    }
    if (health.peer_completed)
    {
        stage = 2;
        title = "Connection ready...";
        detail = "Waiting for the first video packets";
    }
    if (counters.access_units > 0)
    {
        stage = 2;
        title = counters.decoded_frames > 0
            ? "Preparing the first clean frame..."
            : "Decoding the video stream...";
        detail = counters.decoded_frames > 0
            ? "The game image will appear automatically"
            : "Video is arriving and the decoder is synchronizing";
    }

    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillColor(vg, nvgRGB(10, 13, 17));
    nvgFill(vg);

    const float glow_x = x + width * 0.5f + static_cast<float>(std::sin(seconds * 0.45)) * 85.0f;
    const float glow_y = y + height * 0.58f + static_cast<float>(std::cos(seconds * 0.38)) * 42.0f;
    nvgBeginPath(vg);
    nvgCircle(vg, glow_x, glow_y, 210.0f);
    nvgFillColor(vg, nvgRGBA(42, 204, 116, 12));
    nvgFill(vg);

    const float panel_width = std::min(680.0f, width - 72.0f);
    const float panel_height = 410.0f;
    const float panel_x = x + (width - panel_width) * 0.5f;
    const float panel_y = y + (height - panel_height) * 0.5f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, panel_x, panel_y, panel_width, panel_height, 20.0f);
    nvgFillColor(vg, nvgRGBA(16, 18, 22, 248));
    nvgFill(vg);

    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, nvgRGB(77, 218, 130));
    nvgText(vg, panel_x + 42.0f, panel_y + 45.0f, "NOW LOADING", nullptr);
    nvgFontSize(vg, 28.0f);
    nvgFillColor(vg, nvgRGB(241, 245, 247));
    const std::string game = game_title_.empty() ? "GeForce NOW session" : game_title_;
    nvgText(vg, panel_x + 42.0f, panel_y + 82.0f, game.c_str(), nullptr);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, nvgRGB(142, 150, 160));
    nvgText(vg, panel_x + 42.0f, panel_y + 112.0f, "GeForce NOW cloud stream", nullptr);

    static constexpr const char* kSteps[] = {"QUEUE", "SETUP", "READY"};
    const float rail_y = panel_y + 178.0f;
    const float first_x = panel_x + 88.0f;
    const float gap = (panel_width - 176.0f) * 0.5f;
    nvgBeginPath(vg);
    nvgRect(vg, first_x, rail_y - 2.0f, gap * 2.0f, 4.0f);
    nvgFillColor(vg, nvgRGB(37, 41, 47));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, first_x, rail_y - 2.0f, gap * std::min(stage, 2), 4.0f);
    nvgFillColor(vg, nvgRGB(77, 218, 130));
    nvgFill(vg);

    const float pulse = 0.5f + 0.5f * static_cast<float>(std::sin(seconds * 3.1));
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    for (int i = 0; i < 3; ++i)
    {
        const bool complete = i < stage;
        const bool active = i == stage;
        const float cx = first_x + i * gap;
        if (active)
        {
            nvgBeginPath(vg);
            nvgCircle(vg, cx, rail_y, 29.0f + pulse * 3.0f);
            nvgFillColor(vg, nvgRGBA(77, 218, 130, 22 + static_cast<int>(pulse * 24.0f)));
            nvgFill(vg);
        }
        nvgBeginPath(vg);
        nvgCircle(vg, cx, rail_y, 22.0f);
        nvgFillColor(vg, complete || active ? nvgRGB(77, 218, 130) : nvgRGB(24, 28, 33));
        nvgFill(vg);
        nvgStrokeWidth(vg, 2.0f);
        nvgStrokeColor(vg, complete || active ? nvgRGB(108, 235, 153) : nvgRGB(43, 48, 55));
        nvgStroke(vg);
        nvgFontSize(vg, 16.0f);
        nvgFillColor(vg, complete || active ? nvgRGB(8, 35, 22) : nvgRGB(95, 101, 111));
        const std::string number = std::to_string(i + 1);
        nvgText(vg, cx, rail_y, number.c_str(), nullptr);
        nvgFontSize(vg, 12.0f);
        nvgFillColor(vg, complete || active ? nvgRGB(225, 232, 229) : nvgRGB(91, 96, 105));
        nvgText(vg, cx, rail_y + 42.0f, kSteps[i], nullptr);
    }

    const float spinner_x = panel_x + panel_width * 0.5f;
    const float spinner_y = panel_y + 278.0f;
    const float angle = static_cast<float>(
        std::fmod(seconds * 3.0, static_cast<double>(kPi) * 2.0));
    nvgBeginPath(vg);
    nvgCircle(vg, spinner_x, spinner_y, 19.0f);
    nvgStrokeWidth(vg, 5.0f);
    nvgStrokeColor(vg, nvgRGB(35, 43, 48));
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgArc(vg, spinner_x, spinner_y, 19.0f, angle, angle + kPi * 1.42f, NVG_CW);
    nvgStrokeWidth(vg, 5.0f);
    nvgLineCap(vg, NVG_ROUND);
    nvgStrokeColor(vg, nvgRGB(77, 218, 130));
    nvgStroke(vg);
    const float head_angle = angle + kPi * 1.42f;
    nvgBeginPath(vg);
    nvgCircle(vg, spinner_x + std::cos(head_angle) * 19.0f,
              spinner_y + std::sin(head_angle) * 19.0f, 3.4f);
    nvgFillColor(vg, nvgRGB(123, 242, 166));
    nvgFill(vg);

    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 20.0f);
    nvgFillColor(vg, nvgRGB(238, 242, 245));
    nvgText(vg, spinner_x, panel_y + 330.0f, title.c_str(), nullptr);
    nvgFontSize(vg, 15.0f);
    nvgFillColor(vg, nvgRGB(145, 153, 164));
    nvgText(vg, spinner_x, panel_y + 361.0f, detail.c_str(), nullptr);
}

void StreamView::UpdatePerformanceCounter(const VideoPerformanceCounters& counters) {
    const auto now = std::chrono::steady_clock::now();
    if (fps_window_started_.time_since_epoch().count() == 0) {
        fps_window_started_ = now;
        previous_video_counters_ = counters;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - fps_window_started_);
    if (elapsed >= std::chrono::milliseconds(750)) {
        const float scale = elapsed.count() > 0 ? 1000.0f / static_cast<float>(elapsed.count()) : 0.0f;
        const auto delta = [](uint64_t current, uint64_t previous) {
            return current >= previous ? current - previous : uint64_t {0};
        };
        incoming_fps_ = static_cast<float>(
            delta(counters.access_units, previous_video_counters_.access_units)) * scale;
        decoded_fps_ = static_cast<float>(
            delta(counters.decoded_frames, previous_video_counters_.decoded_frames)) * scale;
        presented_fps_ = static_cast<float>(
            delta(counters.presented_frames, previous_video_counters_.presented_frames)) * scale;
        stream_bitrate_mbps_ = static_cast<float>(
            delta(counters.access_unit_bytes, previous_video_counters_.access_unit_bytes)) *
            scale * 8.0f / 1000000.0f;
        network_rtt_ms_ = session_ ? session_->get_network_rtt_ms() : -1;
        previous_video_counters_ = counters;
        fps_window_started_ = now;
    }
}

void StreamView::DrawPerformanceOverlay(NVGcontext* vg, float x, float y) {
    char text[128];
    if (network_rtt_ms_ >= 0) {
        std::snprintf(
            text, sizeof(text), "FPS  %.0f     BITRATE  %.1f Mbps     PING  %d ms",
            presented_fps_, stream_bitrate_mbps_, network_rtt_ms_);
    } else {
        std::snprintf(
            text, sizeof(text), "FPS  %.0f     BITRATE  %.1f Mbps     PING  -- ms",
            presented_fps_, stream_bitrate_mbps_);
    }

    const float box_x = x + 16.0f;
    const float box_y = y + 16.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, 520.0f, 46.0f, 8.0f);
    nvgFillColor(vg, nvgRGBA(8, 12, 14, 205));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x + 6.0f, box_y + 7.0f, 4.0f, 32.0f, 2.0f);
    nvgFillColor(vg, nvgRGB(55, 220, 125));
    nvgFill(vg);

    nvgFontSize(vg, 18.0f);
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgFillColor(vg, nvgRGB(245, 250, 247));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, box_x + 20.0f, box_y + 23.0f, text, nullptr);
}

void StreamView::DrawNteAutoLoginStatus(
    NVGcontext* vg, float x, float y, float width, float height,
    std::chrono::steady_clock::time_point now) {
    if (!is_nte_session_ || now >= nte_status_until_)
        return;

    std::string text = nte_status_;
    if (text.empty()) {
        text = nte_credentials_.valid()
            ? "NTE Auto-login ready: press L + X"
            : "Configure NTE Auto-login on the game page";
    }
    if (nte_stage_ != NteAutoLoginStage::Idle)
        text += "  |  B Cancel";

    const float box_width = std::min(560.0f, width - 36.0f);
    const float box_height = 46.0f;
    const float box_x = x + 18.0f;
    const float box_y = y + height - box_height - 18.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, box_width, box_height, 11.0f);
    nvgFillColor(vg, nvgRGBA(7, 12, 14, 220));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, 6.0f, box_height, 3.0f);
    nvgFillColor(vg, nvgRGB(55, 220, 125));
    nvgFill(vg);
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgFontSize(vg, 19.0f);
    nvgFillColor(vg, nvgRGB(246, 250, 248));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, box_x + 20.0f, box_y + box_height * 0.5f, text.c_str(), nullptr);
}

void StreamView::UpdateSessionLimitNotice(std::chrono::steady_clock::time_point now) {
    if (!free_tier_session_ || stream_started_at_.time_since_epoch().count() == 0)
        return;

    constexpr auto kFreeSessionLimit = std::chrono::hours(1);
    const auto elapsed = now - stream_started_at_;
    const auto remaining = elapsed < kFreeSessionLimit
        ? std::chrono::duration_cast<std::chrono::seconds>(kFreeSessionLimit - elapsed)
        : std::chrono::seconds(0);

    if (elapsed >= kFreeSessionLimit ||
        (session_ && session_->is_terminal() && elapsed >= std::chrono::minutes(59))) {
        BeginStreamEnd(opennow::StreamEndReason::FreeSessionEnded, now);
        return;
    }

    if (!one_minute_warning_shown_ && remaining <= std::chrono::seconds(60)) {
        one_minute_warning_shown_ = true;
        five_minute_warning_shown_ = true;
        session_limit_notice_ = SessionLimitNotice::OneMinute;
        session_notice_visible_until_ = now + std::chrono::seconds(10);
        if (session_)
            session_->record_ui_event("free_session_warning remainingSeconds=60");
    } else if (!five_minute_warning_shown_ && remaining <= std::chrono::minutes(5)) {
        five_minute_warning_shown_ = true;
        session_limit_notice_ = SessionLimitNotice::FiveMinutes;
        session_notice_visible_until_ = now + std::chrono::seconds(8);
        if (session_)
            session_->record_ui_event("free_session_warning remainingSeconds=300");
    }
}

void StreamView::BeginStreamEnd(
    opennow::StreamEndReason reason,
    std::chrono::steady_clock::time_point now) {
    if (reason == opennow::StreamEndReason::None ||
        stream_end_reason_ != opennow::StreamEndReason::None)
        return;

    stream_end_reason_ = reason;
    stream_end_started_at_ = now;
    stream_auto_exit_at_ = now + std::chrono::seconds(15);
    if (reason == opennow::StreamEndReason::FreeSessionEnded) {
        session_limit_ended_ = true;
        session_limit_notice_ = SessionLimitNotice::Ended;
    }

    std::string event = "stream_end reason=";
    switch (reason) {
        case opennow::StreamEndReason::FreeSessionEnded:
            event += "free_session_limit";
            break;
        case opennow::StreamEndReason::NetworkLost:
            event += "network_lost";
            break;
        case opennow::StreamEndReason::ServerDisconnected:
            event += "server_disconnected";
            break;
        case opennow::StreamEndReason::ConnectionFailed:
            event += "connection_failed";
            break;
        case opennow::StreamEndReason::StreamEnded:
            event += "server_ended_stream";
            break;
        case opennow::StreamEndReason::VideoTimedOut:
            event += "video_timeout";
            break;
        case opennow::StreamEndReason::None:
            return;
    }
    event += " autoExitSeconds=15";
    if (session_) {
        session_->record_ui_event(event);
        // Let transport and decoder workers finish during the 15-second
        // notice. Destruction then cannot leave Borealis input blocked.
        session_->request_stop();
    }
}

void StreamView::UpdateStreamEndState(std::chrono::steady_clock::time_point now) {
    if (!session_ || stream_end_reason_ != opennow::StreamEndReason::None)
        return;

    if (last_network_check_at_.time_since_epoch().count() == 0 ||
        now - last_network_check_at_ >= std::chrono::seconds(1)) {
        internet_connected_ = opennow::NetworkUtils::HasInternetConnection();
        last_network_check_at_ = now;
    }

    const StreamTransportHealth health = session_->get_transport_health();
    opennow::StreamEndSignals signals;
    signals.free_tier = free_tier_session_;
    signals.session_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - stream_started_at_);
    signals.internet_connected = internet_connected_;
    signals.peer_completed = health.peer_completed;
    signals.peer_terminal = health.peer_terminal;
    signals.signaling_connected = health.signaling_connected;
    signals.video_started = health.video_started;
    signals.video_idle = health.video_idle;
    BeginStreamEnd(opennow::DetectStreamEnd(signals), now);
}

void StreamView::DrawStreamEndNotice(
    NVGcontext* vg, float x, float y, float width,
    std::chrono::steady_clock::time_point now) {
    if (stream_end_reason_ == opennow::StreamEndReason::None)
        return;

    std::string title;
    std::string detail;
    switch (stream_end_reason_) {
        case opennow::StreamEndReason::FreeSessionEnded:
            title = "Free session ended";
            detail = "The one-hour GeForce NOW limit was reached";
            break;
        case opennow::StreamEndReason::NetworkLost:
            title = "Network connection lost";
            detail = "Check the Switch internet connection";
            break;
        case opennow::StreamEndReason::ServerDisconnected:
            title = "Streaming server disconnected";
            detail = "The connection to the GeForce NOW rig was lost";
            break;
        case opennow::StreamEndReason::ConnectionFailed:
            title = "Stream connection failed";
            detail = "The GeForce NOW rig could not be reached";
            break;
        case opennow::StreamEndReason::StreamEnded:
            title = "Stream ended by server";
            detail = "GeForce NOW closed this streaming session";
            break;
        case opennow::StreamEndReason::VideoTimedOut:
            title = "Video stream timed out";
            detail = "No video packets were received for 15 seconds";
            break;
        case opennow::StreamEndReason::None:
            return;
    }

    const auto remaining_ms = std::max<std::int64_t>(0,
        std::chrono::duration_cast<std::chrono::milliseconds>(stream_auto_exit_at_ - now).count());
    const int remaining_seconds = static_cast<int>((remaining_ms + 999) / 1000);
    detail += "  |  Returning in " + std::to_string(remaining_seconds) + "s";

    const float box_width = std::min(560.0f, width - 36.0f);
    const float box_height = 82.0f;
    const float box_x = x + width - box_width - 18.0f;
    const float box_y = y + 16.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, box_width, box_height, 13.0f);
    nvgFillColor(vg, nvgRGBA(8, 11, 14, 232));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, 7.0f, box_height, 3.5f);
    nvgFillColor(vg, nvgRGB(255, 91, 91));
    nvgFill(vg);
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 23.0f);
    nvgFillColor(vg, nvgRGB(252, 246, 246));
    nvgText(vg, box_x + 22.0f, box_y + 29.0f, title.c_str(), nullptr);
    nvgFontSize(vg, 16.0f);
    nvgFillColor(vg, nvgRGB(188, 195, 204));
    nvgText(vg, box_x + 22.0f, box_y + 59.0f, detail.c_str(), nullptr);
}

void StreamView::DrawSessionLimitNotice(
    NVGcontext* vg, float x, float y, float width,
    std::chrono::steady_clock::time_point now) {
    if (!free_tier_session_ || session_limit_notice_ == SessionLimitNotice::None)
        return;

    constexpr auto kFreeSessionLimit = std::chrono::hours(1);
    const auto elapsed = now - stream_started_at_;
    const int remaining_seconds = static_cast<int>(std::max<int64_t>(0,
        std::chrono::duration_cast<std::chrono::seconds>(kFreeSessionLimit - elapsed).count()));
    const bool prominent = session_limit_notice_ == SessionLimitNotice::Ended ||
                           now < session_notice_visible_until_;
    if (session_limit_notice_ == SessionLimitNotice::FiveMinutes && !prominent)
        return;

    std::string text;
    NVGcolor accent = nvgRGB(255, 184, 58);
    if (session_limit_notice_ == SessionLimitNotice::Ended) {
        text = "Free session ended";
        accent = nvgRGB(255, 91, 91);
    } else if (session_limit_notice_ == SessionLimitNotice::FiveMinutes) {
        text = "Free session: 5 minutes remaining";
    } else {
        text = prominent
            ? "Free session ends in 60 seconds"
            : "Session 0:" + (remaining_seconds < 10 ? std::string("0") : std::string()) +
                std::to_string(remaining_seconds);
        accent = nvgRGB(255, 112, 82);
    }

    const float box_width = prominent ? std::min(390.0f, width - 36.0f) : 190.0f;
    const float box_height = prominent ? 50.0f : 40.0f;
    const float box_x = x + width - box_width - 18.0f;
    const float box_y = y + 16.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, box_width, box_height, prominent ? 12.0f : 9.0f);
    nvgFillColor(vg, nvgRGBA(7, 10, 12, prominent ? 224 : 198));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, box_x, box_y, 6.0f, box_height, 3.0f);
    nvgFillColor(vg, accent);
    nvgFill(vg);

    nvgFontSize(vg, prominent ? 22.0f : 21.0f);
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgFillColor(vg, nvgRGB(250, 252, 250));
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgText(vg, box_x + box_width * 0.5f, box_y + box_height * 0.5f, text.c_str(), nullptr);
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
        bool nte_owned_input = false;
        if (is_nte_session_ &&
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

        const bool keyboard_combo = state.buttons[brls::BUTTON_BACK] &&
                                    state.buttons[brls::BUTTON_Y];
        bool keyboard_owned_input = keyboard_visible_ || nte_owned_input ||
            stream_end_reason_ != opennow::StreamEndReason::None;
        if (keyboard_combo && !keyboard_combo_was_down_ && !nte_owned_input &&
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
        const bool plus_down = state.buttons[brls::BUTTON_START];
        if (plus_down && !plus_was_down_) {
            plus_pressed_at_ = now;
            plus_long_press_ = false;
        }
        if (plus_down && !plus_long_press_ &&
            now - plus_pressed_at_ >= std::chrono::milliseconds(500)) {
            plus_long_press_ = true;
        }
        if (!plus_down && plus_was_down_ && !plus_long_press_) {
            // Keep Start pending until SCTP accepts it. Once delivered, keep
            // it active for multiple reports and then send a clean release.
            start_pulse_.Queue(now);
            gamepad_state_initialized_ = false;
            last_gamepad_report_ = {};
        }
        plus_was_down_ = plus_down;

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

        const bool start_active = start_pulse_.IsActive(now);

        uint16_t buttons = 0;
        if (state.buttons[brls::BUTTON_UP]) buttons |= 0x0001;
        if (state.buttons[brls::BUTTON_DOWN]) buttons |= 0x0002;
        if (state.buttons[brls::BUTTON_LEFT]) buttons |= 0x0004;
        if (state.buttons[brls::BUTTON_RIGHT]) buttons |= 0x0008;
        if (start_active) buttons |= 0x0010;
        if (state.buttons[brls::BUTTON_BACK]) buttons |= 0x0020;
        if (plus_down && plus_long_press_) buttons |= 0x0400;
        if (state.buttons[brls::BUTTON_LSB]) buttons |= 0x0040;
        if (state.buttons[brls::BUTTON_RSB]) buttons |= 0x0080;
        if (state.buttons[brls::BUTTON_LB]) buttons |= 0x0100;
        if (state.buttons[brls::BUTTON_RB]) buttons |= 0x0200;
        buttons |= opennow::MapFaceButtons(
            controller_layout_,
            state.buttons[brls::BUTTON_A],
            state.buttons[brls::BUTTON_B] && !suppress_b_until_release_,
            state.buttons[brls::BUTTON_X],
            state.buttons[brls::BUTTON_Y]);
        
        float lx = state.axes[brls::LEFT_X];
        float ly = state.axes[brls::LEFT_Y];
        float rx = state.axes[brls::RIGHT_X];
        float ry = state.axes[brls::RIGHT_Y];

        ApplyRadialDeadzone(lx, ly);
        ApplyRadialDeadzone(rx, ry);
        const uint8_t left_trigger = state.buttons[brls::BUTTON_LT] ? 0xff : 0x00;
        const uint8_t right_trigger = state.buttons[brls::BUTTON_RT] ? 0xff : 0x00;
        const int16_t qlx = QuantizeAxis(lx);
        const int16_t qly = QuantizeAxis(ly);
        const int16_t qrx = QuantizeAxis(rx);
        const int16_t qry = QuantizeAxis(ry);
        const bool state_changed = !gamepad_state_initialized_ ||
            buttons != last_gamepad_buttons_ ||
            left_trigger != last_left_trigger_ || right_trigger != last_right_trigger_ ||
            qlx != last_lx_ || qly != last_ly_ || qrx != last_rx_ || qry != last_ry_;
        const bool keepalive_due = last_gamepad_report_.time_since_epoch().count() == 0 ||
            now - last_gamepad_report_ >= std::chrono::milliseconds(100);

        if (state_changed || keepalive_due) {
            const bool delivered = session_->send_gamepad_input(
                buttons, left_trigger, right_trigger, lx, ly, rx, ry);
            if (delivered) {
                gamepad_state_initialized_ = true;
                last_gamepad_buttons_ = buttons;
                last_left_trigger_ = left_trigger;
                last_right_trigger_ = right_trigger;
                last_lx_ = qlx;
                last_ly_ = qly;
                last_rx_ = qrx;
                last_ry_ = qry;
                last_gamepad_report_ = now;
                if (start_active && start_pulse_.OnReportDelivered(now)) {
                    gamepad_state_initialized_ = false;
                }
            } else {
                // Retry on the next frame instead of treating a blocked or
                // failed report as delivered after a long idle period.
                gamepad_state_initialized_ = false;
            }
        }

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
