#include "StreamView.hpp"
#include "keyboard_input_policy.hpp"
#include "nte_autologin_log.hpp"
#include "stream_diagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

int16_t StreamView::ClampMouseDelta(float value)
{
    return static_cast<int16_t>(std::clamp<long>(std::lround(value), -32768L, 32767L));
}

#ifdef __SWITCH__
StreamView* StreamView::active_keyboard_view_ = nullptr;
#endif

void StreamView::SendKeyboardCharacter(char character) {
    if (!session_)
        return;

    opennow::input::KeyboardStroke stroke {};
    if (character == '\b')
        stroke = {0x08, 0x0e, 0};
    else if (character == '\n' || character == '\r')
        stroke = {0x0d, 0x1c, 0};
    else if (!opennow::input::MapAsciiKey(character, stroke)) {
        session_->record_ui_event("keyboard unsupported UTF-8 byte=" +
                                  std::to_string(static_cast<unsigned char>(character)));
        return;
    }

    SendKeyboardStroke(stroke);
}

void StreamView::SendKeyboardStroke(opennow::input::KeyboardStroke stroke) {
    if (!session_)
        return;

    const auto tap = opennow::input::MakeKeyboardTap(stroke);
    for (std::size_t i = 0; i < tap.size; ++i) {
        const auto& event = tap.events[i];
        session_->send_keyboard_key(event.stroke.keycode, event.stroke.scancode,
                                    event.stroke.modifiers, event.pressed);
    }
}

void StreamView::SendKeyboardShortcut(opennow::input::KeyboardShortcut shortcut) {
    if (!keyboard_visible_ || !session_ ||
        !session_->get_transport_health().peer_completed)
        return;

    SendKeyboardStroke(opennow::input::MapKeyboardShortcut(shortcut));
    keyboard_text_.clear();
#ifdef __SWITCH__
    swkbdInlineSetInputText(&inline_keyboard_, "");
    swkbdInlineSetCursorPos(&inline_keyboard_, 0);
#endif
}

void StreamView::HandleInlineKeyboardInput(
    const brls::ControllerState& state, float x, float y, float width) {
    const bool b_down = state.buttons[brls::BUTTON_B];
    const bool plus_down = state.buttons[brls::BUTTON_START];
    const std::array buttons {
        brls::BUTTON_LT, brls::BUTTON_LB, brls::BUTTON_RB, brls::BUTTON_RT,
        brls::BUTTON_LEFT, brls::BUTTON_UP, brls::BUTTON_RIGHT, brls::BUTTON_DOWN,
    };
    std::uint16_t shortcut_buttons = 0;
    if (state.buttons[brls::BUTTON_BACK]) {
        for (std::size_t i = 0; i < buttons.size(); ++i)
            if (state.buttons[buttons[i]])
                shortcut_buttons |= 1u << i;
    }
    const int shortcut = opennow::input::PollKeyboardShortcut(
        shortcut_buttons, keyboard_shortcut_latched_);

    std::vector<brls::RawTouchState> touches;
    brls::Application::getPlatform()->getInputManager()->updateTouchStates(&touches);
    const auto touch = std::find_if(touches.begin(), touches.end(),
        [](const brls::RawTouchState& value) { return value.pressed; });
    const bool touch_down = touch != touches.end();
    int touch_shortcut = -1;
    if (touch_down && !keyboard_touch_was_down_) {
        for (std::size_t i = 0; i < opennow::input::kKeyboardShortcutControls.size(); ++i) {
            if (opennow::input::KeyboardShortcutBounds(i, x, y, width).Contains(
                    touch->position.x, touch->position.y)) {
                touch_shortcut = static_cast<int>(i);
                break;
            }
        }
    }
    keyboard_touch_was_down_ = touch_down;

    if (plus_down && !keyboard_plus_was_down_)
        HideInlineKeyboard(true);
    else if (b_down && !keyboard_b_was_down_)
        HideInlineKeyboard(false);
    else if (shortcut >= 0 || touch_shortcut >= 0)
        SendKeyboardShortcut(opennow::input::kKeyboardShortcutControls[
            shortcut >= 0 ? shortcut : touch_shortcut].shortcut);
    keyboard_b_was_down_ = b_down;
    keyboard_plus_was_down_ = plus_down;
}

void StreamView::DrawKeyboardShortcuts(NVGcontext* vg, float x, float y, float width) {
    if (!keyboard_visible_)
        return;

    nvgSave(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + 8.0f, y + 8.0f, width - 16.0f, 192.0f, 12.0f);
    nvgFillColor(vg, nvgRGBA(13, 19, 22, 242));
    nvgFill(vg);
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgFontSize(vg, 16.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, nvgRGB(231, 237, 233));
    nvgText(vg, x + 20.0f, y + 29.0f, "REMOTE KEYBOARD", nullptr);
    nvgFontSize(vg, 13.0f);
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgText(vg, x + width - 20.0f, y + 29.0f,
            "Tap a key or use its chord  |  PLUS Enter  |  B Close", nullptr);
    for (std::size_t i = 0; i < opennow::input::kKeyboardShortcutControls.size(); ++i) {
        const auto& control = opennow::input::kKeyboardShortcutControls[i];
        const auto rect = opennow::input::KeyboardShortcutBounds(i, x, y, width);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, rect.x, rect.y, rect.width, rect.height, 7.0f);
        nvgFillColor(vg, nvgRGBA(77, 218, 130, 22));
        nvgFill(vg);
        nvgStrokeWidth(vg, 1.0f);
        nvgStrokeColor(vg, nvgRGBA(77, 218, 130, 85));
        nvgStroke(vg);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(vg, 18.0f);
        nvgFillColor(vg, nvgRGB(231, 237, 233));
        nvgText(vg, rect.x + rect.width * 0.5f, rect.y + 19.0f, control.label, nullptr);
        nvgFontSize(vg, 12.0f);
        nvgFillColor(vg, nvgRGB(122, 235, 164));
        nvgText(vg, rect.x + rect.width * 0.5f, rect.y + 41.0f, control.chord, nullptr);
    }
    nvgFontSize(vg, 12.0f);
    nvgFillColor(vg, nvgRGB(188, 198, 202));
    nvgText(vg, x + width * 0.5f, y + 184.0f,
            "Win+D Desktop  /  Win+E Explorer  /  Win+R Run  /  Win+Tab Task view", nullptr);
    nvgRestore(vg);
}

void StreamView::SendNteClick(float normalized_x, float normalized_y) {
    if (!session_) {
        opennow::AppendNteAutoLoginLog("CLICK skipped reason=no_session");
        return;
    }

    const int stream_width = std::max(1, session_->negotiated_stream_width());
    const int stream_height = std::max(1, session_->negotiated_stream_height());
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

    const int stream_width = std::max(1, session_->negotiated_stream_width());
    const int stream_height = std::max(1, session_->negotiated_stream_height());
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
    SendKeyboardStroke({'A', 0x1e, kControl});
    SendKeyboardStroke({0x08, 0x0e, 0});
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
    if (!keyboard_visible_ || !session_ ||
        !session_->get_transport_health().peer_completed)
        return;
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
    if (keyboard_visible_ || !session_ ||
        !session_->get_transport_health().peer_completed)
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
    keyboard_shortcut_latched_ = true;
    keyboard_touch_was_down_ = true;
    ResetControllerDeliveryState();
    if (touch_was_down_) {
        session_->send_mouse_left_button(false);
        touch_was_down_ = false;
    }
    SendNeutralControllerReports();
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
    keyboard_shortcut_latched_ = false;
    keyboard_touch_was_down_ = false;
    ResetControllerDeliveryState();
    suppress_b_until_release_ = true;
    keyboard_release_guard_ = true;
    if (session_)
        session_->record_ui_event(send_enter ? "keyboard submitted" : "keyboard closed");
#else
    (void)send_enter;
#endif
}

void StreamView::UpdateInlineKeyboard() {
#ifdef __SWITCH__
    if (!keyboard_launched_)
        return;
    if (keyboard_visible_ && (!session_ ||
        !session_->get_transport_health().peer_completed))
        HideInlineKeyboard(false);
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
    if (!active_keyboard_view_ || !active_keyboard_view_->keyboard_visible_)
        return;
    active_keyboard_view_->HandleKeyboardText(text);
    active_keyboard_view_->HideInlineKeyboard(true);
}

void StreamView::KeyboardCancelCallback() {
    if (!active_keyboard_view_)
        return;
    active_keyboard_view_->HideInlineKeyboard(false);
}
#endif
