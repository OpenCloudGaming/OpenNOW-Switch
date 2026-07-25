#include "qr_login_dialog.hpp"

#include "app_state.hpp"
#include "localization.hpp"
#include "qrcodegen.h"

#include <algorithm>
#include <utility>

namespace opennow
{

QrLoginDialog::QrLoginDialog(
    const LoginProvider& provider,
    const GfnClient& client,
    std::function<void()> on_success)
    : brls::Box(brls::Axis::COLUMN),
      provider_(provider),
      client_(client),
      on_success_(std::move(on_success))
{
    setPadding(24, 64, 24, 64);
    setBackgroundColor(nvgRGB(11, 12, 15));
    setAlignItems(brls::AlignItems::CENTER);

    auto* header = new brls::Header();
    const std::string provider_name =
        provider_.display_name.empty() ? "GeForce NOW" : provider_.display_name;
    header->setTitle(Tr("Sign in with QR code") + "  /  " + provider_name);
    header->setMarginBottom(14);
    addView(header);

    status_label_ = new brls::Label();
    status_label_->setText(Tr("Preparing secure QR login..."));
    status_label_->setFontSize(29);
    status_label_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    status_label_->setTextColor(nvgRGB(244, 247, 250));
    status_label_->setMarginBottom(7);
    addView(status_label_);

    instruction_label_ = new brls::Label();
    instruction_label_->setText(
        Tr("Scan the code with your phone, approve the provider sign-in, and return here."));
    instruction_label_->setFontSize(17);
    instruction_label_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    instruction_label_->setTextColor(nvgRGB(145, 153, 165));
    instruction_label_->setMarginBottom(8);
    addView(instruction_label_);

    auto* qr_space = new brls::Box(brls::Axis::COLUMN);
    qr_space->setHeight(355);
    qr_space->setWidth(420);
    qr_space->setMarginBottom(4);
    addView(qr_space);

    user_code_label_ = new brls::Label();
    user_code_label_->setText("");
    user_code_label_->setFontSize(22);
    user_code_label_->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    user_code_label_->setTextColor(nvgRGB(96, 236, 136));
    user_code_label_->setMarginBottom(14);
    addView(user_code_label_);

    auto* actions = new brls::Box(brls::Axis::ROW);
    actions->setWidth(520);
    actions->setHeight(54);
    addView(actions);

    retry_button_ = new brls::Button();
    retry_button_->setGrow(1.0f);
    retry_button_->setHeight(52);
    retry_button_->setText(Tr("New QR code"));
    retry_button_->setMarginRight(10);
    retry_button_->registerClickAction(
        [this](brls::View* view) { return RetryLogin(view); });
    actions->addView(retry_button_);

    cancel_button_ = new brls::Button();
    cancel_button_->setGrow(1.0f);
    cancel_button_->setHeight(52);
    cancel_button_->setStyle(&brls::BUTTONSTYLE_BORDERED);
    cancel_button_->setText(Tr("Cancel login"));
    cancel_button_->registerClickAction(
        [this](brls::View* view) { return CancelLogin(view); });
    actions->addView(cancel_button_);
}

QrLoginDialog::~QrLoginDialog()
{
    lifetime_guard_->store(false);
    is_cancelled_.store(true);
    if (worker_.joinable())
        worker_.join();
}

void QrLoginDialog::willAppear(bool reset_state)
{
    brls::Box::willAppear(reset_state);
    if (!started_)
    {
        started_ = true;
        StartLogin();
    }
    if (retry_button_)
        brls::Application::giveFocus(retry_button_);
}

void QrLoginDialog::willDisappear(bool reset_state)
{
    brls::Box::willDisappear(reset_state);
    is_cancelled_.store(true);
}

bool QrLoginDialog::RetryLogin(brls::View* view)
{
    (void)view;
    if (login_active_.load())
        return true;
    StartLogin();
    return true;
}

bool QrLoginDialog::CancelLogin(brls::View* view)
{
    (void)view;
    if (!login_active_.load())
    {
        brls::Application::popActivity();
        return true;
    }
    is_cancelled_.store(true);
    status_label_->setText(Tr("Cancelling QR login..."));
    instruction_label_->setText(Tr("The current device code will be discarded."));
    return true;
}

void QrLoginDialog::StartLogin()
{
    if (login_active_.exchange(true))
        return;
    if (worker_.joinable())
        worker_.join();

    is_cancelled_.store(false);
    status_label_->setText(Tr("Preparing secure QR login..."));
    instruction_label_->setText(
        Tr("Requesting a short-lived device code for ") +
        (provider_.display_name.empty() ? "GeForce NOW" : provider_.display_name) + ".");
    user_code_label_->setText("");
    {
        std::lock_guard<std::mutex> lock(qr_mutex_);
        qr_data_.clear();
        qr_size_ = 0;
    }

    const auto guard = lifetime_guard_;
    worker_ = std::thread([this, guard]() {
        try
        {
            AuthSession session = client_.LoginWithQrCode(
                provider_,
                [this, guard](const QrLoginChallenge& challenge) {
                    if (!guard->load() || is_cancelled_.load())
                        return;
                    SetQrCode(challenge.verification_uri_complete);
                    brls::sync([this, guard, challenge]() {
                        if (!guard->load() || is_cancelled_.load())
                            return;
                        status_label_->setText(Tr("Scan to sign in"));
                        instruction_label_->setText(Tr(
                            "Open your camera, scan the QR code, then approve the provider request."));
                        user_code_label_->setText(
                            Tr("Code: ") + challenge.user_code);
                    });
                },
                [this, guard]() {
                    return is_cancelled_.load() || !guard->load();
                });

            if (is_cancelled_.load() || !guard->load())
                return;
            brls::sync([this, guard, session = std::move(session)]() mutable {
                if (guard->load())
                    CompleteLogin(session);
            });
        }
        catch (const std::exception& error)
        {
            const std::string message = error.what();
            brls::sync([this, guard, message]() {
                if (!guard->load())
                    return;
                login_active_.store(false);
                status_label_->setText(
                    is_cancelled_.load() ? Tr("QR login cancelled") : Tr("QR login failed"));
                instruction_label_->setText(
                    is_cancelled_.load()
                        ? Tr("Choose New QR code when you are ready to try again.")
                        : message);
                user_code_label_->setText("");
                std::lock_guard<std::mutex> lock(qr_mutex_);
                qr_data_.clear();
                qr_size_ = 0;
            });
        }
    });
}

void QrLoginDialog::CompleteLogin(const AuthSession& session)
{
    AuthSession active_session = session;
    active_session.persistence_enabled = true;
    client_.SaveSession(active_session);
    AppState::Instance().SetSession(active_session);
    login_active_.store(false);
    brls::Application::notify("Account connected: " + session.user.display_name);
    brls::Application::popActivity();
    if (on_success_)
        on_success_();
}

void QrLoginDialog::SetQrCode(const std::string& url)
{
    std::vector<uint8_t> temporary(qrcodegen_BUFFER_LEN_MAX);
    std::vector<uint8_t> encoded(qrcodegen_BUFFER_LEN_MAX);
    const bool success = qrcodegen_encodeText(
        url.c_str(), temporary.data(), encoded.data(), qrcodegen_Ecc_LOW,
        qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true);

    std::lock_guard<std::mutex> lock(qr_mutex_);
    if (!success)
    {
        qr_data_.clear();
        qr_size_ = 0;
        return;
    }
    qr_size_ = qrcodegen_getSize(encoded.data());
    qr_data_ = std::move(encoded);
}

void QrLoginDialog::draw(
    NVGcontext* vg, float x, float y, float width, float height,
    brls::Style style, brls::FrameContext* ctx)
{
    brls::Box::draw(vg, x, y, width, height, style, ctx);

    std::lock_guard<std::mutex> lock(qr_mutex_);
    if (qr_size_ <= 0 || qr_data_.empty())
        return;

    const float cell_size = std::max(3.0f, std::min(6.0f, 300.0f / qr_size_));
    const float qr_pixels = qr_size_ * cell_size;
    const float start_x = x + (width - qr_pixels) * 0.5f;
    const float start_y = y + 190.0f;

    nvgBeginPath(vg);
    nvgRect(vg, start_x - 14, start_y - 14, qr_pixels + 28, qr_pixels + 28);
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgFill(vg);

    nvgBeginPath(vg);
    for (int row = 0; row < qr_size_; ++row)
    {
        for (int column = 0; column < qr_size_; ++column)
        {
            if (qrcodegen_getModule(qr_data_.data(), column, row))
            {
                nvgRect(vg, start_x + column * cell_size,
                        start_y + row * cell_size, cell_size, cell_size);
            }
        }
    }
    nvgFillColor(vg, nvgRGB(0, 0, 0));
    nvgFill(vg);
}

} // namespace opennow
