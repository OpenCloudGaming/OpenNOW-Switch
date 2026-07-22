#pragma once

#include "gfn_client.hpp"
#include "models.hpp"

#include <borealis.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace opennow
{

class QrLoginDialog : public brls::Box
{
  public:
    QrLoginDialog(
        const LoginProvider& provider,
        const GfnClient& client,
        std::function<void()> on_success = nullptr);
    ~QrLoginDialog() override;

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;
    void willAppear(bool reset_state) override;
    void willDisappear(bool reset_state) override;

  private:
    bool RetryLogin(brls::View* view);
    bool CancelLogin(brls::View* view);
    void StartLogin();
    void CompleteLogin(const AuthSession& session);
    void SetQrCode(const std::string& url);

    LoginProvider provider_;
    const GfnClient& client_;
    brls::Label* status_label_ = nullptr;
    brls::Label* instruction_label_ = nullptr;
    brls::Label* user_code_label_ = nullptr;
    brls::Button* retry_button_ = nullptr;
    brls::Button* cancel_button_ = nullptr;

    std::atomic<bool> is_cancelled_ {false};
    std::atomic<bool> login_active_ {false};
    bool started_ = false;
    std::shared_ptr<std::atomic<bool>> lifetime_guard_ =
        std::make_shared<std::atomic<bool>>(true);
    std::thread worker_;
    std::function<void()> on_success_;

    std::mutex qr_mutex_;
    std::vector<uint8_t> qr_data_;
    int qr_size_ = 0;
};

} // namespace opennow
