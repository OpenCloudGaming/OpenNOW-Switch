#pragma once

#include <borealis.hpp>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <optional>
#include <memory>
#include "gfn_client.hpp"
#include "models.hpp"

namespace opennow {

class QrLoginDialog : public brls::Box {
public:
    QrLoginDialog(const LoginProvider& provider, const GfnClient& client, std::function<void()> on_success = nullptr);
    ~QrLoginDialog() override;

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) override;
    void willAppear(bool resetState) override;
    void willDisappear(bool resetState) override;

private:
    bool StartDeviceLogin(brls::View* view);
    bool StartDifferentAccountLogin(brls::View* view);
    bool StartQrLogin(brls::View* view);
    bool ToggleRememberLogin(brls::View* view);
    bool ToggleRememberPassword(brls::View* view);
    bool CancelActiveLogin(brls::View* view);
    bool StartNativeLogin(bool force_prompt);
    static std::string PromptText(const std::string& title, const std::string& initial, bool password, bool numeric = false);
    void ShowEmailWaitingDialog(const std::string& status);
    void CloseEmailWaitingDialog(std::function<void()> after_close = {});
    void StartQrLoginProcess();
    void CompleteLogin(const AuthSession& session);
    void GenerateQrCode(const std::string& url);

    LoginProvider provider_;
    const GfnClient& client_;
    brls::Label* status_label_;
    brls::Label* instruction_label_;
    brls::Label* url_label_;
    brls::Button* remember_button_;
    brls::Button* remember_password_button_;
    brls::Button* device_login_button_;
    brls::Button* different_account_button_;
    brls::Button* cancel_login_button_;
    brls::Dialog* email_waiting_dialog_ = nullptr;
    brls::Label* email_waiting_label_ = nullptr;
    brls::Button* qr_login_button_;

    std::atomic<bool> is_cancelled_{false};
    std::atomic<bool> login_active_{false};
    bool remember_login_ = true;
    bool remember_password_ = false;
    std::optional<NativeCredentials> saved_credentials_;
    std::shared_ptr<std::atomic<bool>> lifetime_guard_ =
        std::make_shared<std::atomic<bool>>(true);
    std::thread worker_;
    std::string auth_url_;
    std::function<void()> on_success_;
    
    std::vector<uint8_t> qr_data_;
    int qr_size_ = 0;
};

} // namespace opennow
