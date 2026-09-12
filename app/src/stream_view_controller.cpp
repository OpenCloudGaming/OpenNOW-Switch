#include "StreamView.hpp"
#include "controller_layout.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{

void ApplyRadialDeadzone(float& x, float& y)
{
    constexpr float deadzone = 0.12f;
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= deadzone)
    {
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
    value = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lround(value * 32767.0f));
}

} // namespace

void StreamView::PollControllerStates(std::chrono::steady_clock::time_point now)
{
    std::array<bool, opennow::input::kRemoteControllerCount> connected {};
    std::array<brls::ControllerState, opennow::input::kRemoteControllerCount> states {};
    std::array<bool, opennow::input::kSwitchControllerSourceCount> sources_connected {};
    std::array<brls::ControllerState, opennow::input::kSwitchControllerSourceCount> source_states {};

#ifdef __SWITCH__
    for (std::size_t source = 0; source < switch_controller_sources_.size(); ++source)
    {
        sources_connected[source] = ReadSwitchControllerSource(source, source_states[source]);
    }
#else
    auto* input = brls::Application::getPlatform()->getInputManager();
    const std::size_t count = static_cast<std::size_t>(
        std::max<short>(0, input->getControllersConnectedCount()));
    for (std::size_t index = 0;
         index < std::min(count, opennow::input::kRemoteControllerCount); ++index)
    {
        sources_connected[index + 1] = true;
        input->updateControllerState(&source_states[index + 1], static_cast<int>(index));
    }
#endif

    const auto released = controller_assignments_.Update(sources_connected);
    for (std::size_t source = 0; source < sources_connected.size(); ++source)
    {
        const auto controller = controller_assignments_.ControllerForSource(source);
        if (controller < 0)
            continue;
        connected[static_cast<std::size_t>(controller)] = true;
        states[static_cast<std::size_t>(controller)] = source_states[source];
    }

    if (controller_connections_initialized_)
    {
        for (std::size_t controller = 0; controller < connected.size(); ++controller)
        {
            auto& delivery = controller_delivery_[controller];
            if (released[controller])
            {
                delivery.Reset(true);
                if (session_)
                    session_->record_ui_event(
                        "controller disconnected player=" + std::to_string(controller + 1));
            }
            if (connected[controller] &&
                (!controller_connected_[controller] || released[controller]))
            {
                delivery.Reset();
                QueueControllerConnectedNotice(controller, now);
                if (session_)
                    session_->record_ui_event(
                        "controller connected player=" + std::to_string(controller + 1));
            }
        }
    }
    else
    {
        controller_connections_initialized_ = true;
    }

    controller_connected_ = connected;
    controller_states_ = states;
    UpdateControllerNotice(now);
}

#ifdef __SWITCH__
bool StreamView::ReadSwitchControllerSource(
    std::size_t source, brls::ControllerState& state)
{
    if (source >= switch_controller_sources_.size())
        return false;

    PadState& pad = switch_controller_sources_[source];
    padUpdate(&pad);
    const bool active = source == 0
        ? padIsHandheld(&pad)
        : padIsNpadActive(&pad, static_cast<HidNpadIdType>(source - 1));
    if (!active)
        return false;

    state = {};
    const uint64_t buttons = padGetButtons(&pad);
    const bool full = (padGetStyleSet(&pad) & HidNpadStyleSet_NpadFullCtrl) != 0;
    if (full)
    {
        state.buttons[brls::BUTTON_LT] = buttons & HidNpadButton_ZL;
        state.buttons[brls::BUTTON_LB] = buttons & HidNpadButton_L;
        state.buttons[brls::BUTTON_LSB] = buttons & HidNpadButton_StickL;
        state.buttons[brls::BUTTON_UP] = buttons & HidNpadButton_Up;
        state.buttons[brls::BUTTON_RIGHT] = buttons & HidNpadButton_Right;
        state.buttons[brls::BUTTON_DOWN] = buttons & HidNpadButton_Down;
        state.buttons[brls::BUTTON_LEFT] = buttons & HidNpadButton_Left;
        state.buttons[brls::BUTTON_BACK] = buttons & HidNpadButton_Minus;
        state.buttons[brls::BUTTON_START] = buttons & HidNpadButton_Plus;
        state.buttons[brls::BUTTON_RSB] = buttons & HidNpadButton_StickR;
        state.buttons[brls::BUTTON_Y] = buttons & HidNpadButton_Y;
        state.buttons[brls::BUTTON_B] = buttons & HidNpadButton_B;
        state.buttons[brls::BUTTON_A] = buttons & HidNpadButton_A;
        state.buttons[brls::BUTTON_X] = buttons & HidNpadButton_X;
        state.buttons[brls::BUTTON_RB] = buttons & HidNpadButton_R;
        state.buttons[brls::BUTTON_RT] = buttons & HidNpadButton_ZR;

        const HidAnalogStickState left = padGetStickPos(&pad, 0);
        const HidAnalogStickState right = padGetStickPos(&pad, 1);
        state.axes[brls::LEFT_X] = static_cast<float>(left.x) / 32767.0f;
        state.axes[brls::LEFT_Y] = -static_cast<float>(left.y) / 32767.0f;
        state.axes[brls::RIGHT_X] = static_cast<float>(right.x) / 32767.0f;
        state.axes[brls::RIGHT_Y] = -static_cast<float>(right.y) / 32767.0f;
    }
    else
    {
        state.buttons[brls::BUTTON_LB] = buttons & HidNpadButton_AnySL;
        state.buttons[brls::BUTTON_LSB] =
            buttons & (HidNpadButton_StickL | HidNpadButton_StickR);
        state.buttons[brls::BUTTON_START] =
            buttons & (HidNpadButton_Plus | HidNpadButton_Minus);
        state.buttons[brls::BUTTON_Y] =
            buttons & (HidNpadButton_B | HidNpadButton_Up);
        state.buttons[brls::BUTTON_B] =
            buttons & (HidNpadButton_A | HidNpadButton_Left);
        state.buttons[brls::BUTTON_A] =
            buttons & (HidNpadButton_X | HidNpadButton_Down);
        state.buttons[brls::BUTTON_X] =
            buttons & (HidNpadButton_Y | HidNpadButton_Right);
        state.buttons[brls::BUTTON_RB] = buttons & HidNpadButton_AnySR;

        const HidAnalogStickState left = padGetStickPos(&pad, 0);
        const HidAnalogStickState right = padGetStickPos(&pad, 1);
        const float axis_x =
            -static_cast<float>(left.y) / 32767.0f +
            static_cast<float>(right.y) / 32767.0f;
        const float axis_y =
            -static_cast<float>(left.x) / 32767.0f +
            static_cast<float>(right.x) / 32767.0f;
        if (brls::Application::isSwapHalfJoyconStickToDpad())
        {
            state.buttons[brls::BUTTON_UP] = axis_y < -0.3f;
            state.buttons[brls::BUTTON_DOWN] = axis_y > 0.3f;
            state.buttons[brls::BUTTON_RIGHT] = axis_x > 0.3f;
            state.buttons[brls::BUTTON_LEFT] = axis_x < -0.3f;
        }
        else
        {
            state.axes[brls::LEFT_X] = axis_x;
            state.axes[brls::LEFT_Y] = axis_y;
        }
    }
    return true;
}
#endif

uint16_t StreamView::SendPendingControllerDisconnects()
{
    const uint16_t bitmap = opennow::input::ControllerReportBitmap(
        controller_connected_, controller_delivery_);
    for (std::size_t controller = 0; controller < controller_delivery_.size(); ++controller)
    {
        auto& delivery = controller_delivery_[controller];
        if (!delivery.pending_disconnect)
            continue;
        if (session_->send_gamepad_input(
                static_cast<uint8_t>(controller), bitmap,
                0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f))
        {
            delivery.pending_disconnect = false;
        }
    }
    return opennow::input::ControllerReportBitmap(controller_connected_, controller_delivery_);
}

void StreamView::ObserveControllerSystemButtons(std::chrono::steady_clock::time_point now)
{
    for (std::size_t controller = 0; controller < controller_states_.size(); ++controller)
    {
        if (!controller_connected_[controller] || controller_delivery_[controller].pending_disconnect)
            continue;
        const auto& state = controller_states_[controller];
        controller_delivery_[controller].ObserveSystemButtons(
            state.buttons[brls::BUTTON_START], state.buttons[brls::BUTTON_BACK], now);
    }
}

void StreamView::SendControllerInputs(std::chrono::steady_clock::time_point now)
{
    if (!session_)
        return;

    const uint16_t bitmap = SendPendingControllerDisconnects();
    for (std::size_t controller = 0; controller < controller_states_.size(); ++controller)
    {
        if (!controller_connected_[controller] || controller_delivery_[controller].pending_disconnect)
            continue;

        const brls::ControllerState& state = controller_states_[controller];
        auto& delivery = controller_delivery_[controller];
        uint16_t buttons = delivery.SystemButtons(now);
        if (state.buttons[brls::BUTTON_UP]) buttons |= 0x0001;
        if (state.buttons[brls::BUTTON_DOWN]) buttons |= 0x0002;
        if (state.buttons[brls::BUTTON_LEFT]) buttons |= 0x0004;
        if (state.buttons[brls::BUTTON_RIGHT]) buttons |= 0x0008;
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
        const bool state_changed = !delivery.initialized ||
            buttons != delivery.last_buttons ||
            left_trigger != delivery.last_left_trigger ||
            right_trigger != delivery.last_right_trigger ||
            qlx != delivery.last_lx || qly != delivery.last_ly ||
            qrx != delivery.last_rx || qry != delivery.last_ry;
        const bool keepalive_due = delivery.last_report.time_since_epoch().count() == 0 ||
            now - delivery.last_report >= std::chrono::milliseconds(100);
        if (!state_changed && !keepalive_due)
            continue;

        const bool delivered = session_->send_gamepad_input(
            static_cast<uint8_t>(controller), bitmap, buttons,
            left_trigger, right_trigger, lx, ly, rx, ry);
        if (!delivered)
        {
            delivery.initialized = false;
            continue;
        }

        delivery.initialized = true;
        delivery.last_buttons = buttons;
        delivery.last_left_trigger = left_trigger;
        delivery.last_right_trigger = right_trigger;
        delivery.last_lx = qlx;
        delivery.last_ly = qly;
        delivery.last_rx = qrx;
        delivery.last_ry = qry;
        delivery.last_report = now;
        delivery.OnSystemButtonsDelivered(now);
    }
}

void StreamView::UpdateGameplayInputCapture(
    bool captured, std::chrono::steady_clock::time_point now)
{
    if (controller_input_capture_.Update(captured, controller_delivery_) && touch_was_down_)
    {
        if (session_)
            session_->send_mouse_left_button(false);
        touch_was_down_ = false;
    }
    if (!captured || !session_)
        return;

    opennow::input::DeliverNeutralControllerReports(
        controller_connected_, controller_delivery_, now,
        [this](uint8_t controller, uint16_t bitmap) {
            return session_->send_gamepad_input(
                controller, bitmap, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f);
        });
}

void StreamView::QueueControllerConnectedNotice(
    std::size_t controller, std::chrono::steady_clock::time_point now)
{
    const std::string text = "Controller " + std::to_string(controller + 1) + " connected";
    UpdateControllerNotice(now);
    if (controller_notice_text_.empty())
    {
        controller_notice_text_ = text;
        controller_notice_visible_until_ = now + std::chrono::seconds(3);
    }
    else
    {
        // Controller flapping must not create an unbounded UI queue on the
        // streaming path. One queued notice per supported player is enough.
        if (controller_notice_queue_.size() < opennow::input::kRemoteControllerCount)
            controller_notice_queue_.push_back(text);
    }
}

void StreamView::UpdateControllerNotice(std::chrono::steady_clock::time_point now)
{
    if (!controller_notice_text_.empty() && now >= controller_notice_visible_until_)
        controller_notice_text_.clear();
    if (controller_notice_text_.empty() && !controller_notice_queue_.empty())
    {
        controller_notice_text_ = std::move(controller_notice_queue_.front());
        controller_notice_queue_.pop_front();
        controller_notice_visible_until_ = now + std::chrono::seconds(3);
    }
}
