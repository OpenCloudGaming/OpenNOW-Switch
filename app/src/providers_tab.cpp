#include "providers_tab.hpp"

#include "app_state.hpp"
#include "ui_helpers.hpp"
#include "qr_login_dialog.hpp"
#include "localization.hpp"

namespace opennow
{
namespace
{

brls::Label* MakeParagraph(const std::string& text, float bottom_margin = 16.0f)
{
    auto* label = new brls::Label();
    label->setText(Tr(text));
    label->setFontSize(18);
    label->setMarginBottom(bottom_margin);
    return label;
}

class InputBlocker
{
  public:
    InputBlocker()
    {
        brls::Application::blockInputs();
    }

    ~InputBlocker()
    {
        brls::Application::unblockInputs();
    }
};

} // namespace

ProvidersTab::ProvidersTab()
    : brls::Box(brls::Axis::COLUMN)
{
    setPadding(28, 40, 28, 40);

    auto* header = new brls::Header();
    header->setTitle("Providers");
    header->setSubtitle("Add another GeForce NOW account");
    addView(header);

    addView(MakeParagraph(
        "Choose an NVIDIA identity provider. SwitchNOW signs in internally with the Switch keyboard; QR/LAN is only a fallback for CAPTCHA or passkey. Saved tokens refresh automatically.", 20.0f));

    auto* refresh_button = new brls::Button();
    refresh_button->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    refresh_button->setText("Reload providers");
    refresh_button->setMarginBottom(14);
    refresh_button->registerClickAction([this](brls::View* view) {
        ReloadProviders();
        return true;
    });
    addView(refresh_button);

    status_label_ = MakeParagraph("No provider data cached yet.");
    addView(status_label_);

    scrolling_frame_ = new brls::ScrollingFrame();
    scrolling_frame_->setGrow(1.0f);

    list_container_ = new brls::Box(brls::Axis::COLUMN);
    list_container_->setPadding(0, 0, 32, 0);
    scrolling_frame_->setContentView(list_container_);

    addView(scrolling_frame_);
}

void ProvidersTab::willAppear(bool resetState)
{
    brls::Box::willAppear(resetState);

    const auto& state = AppState::Instance();
    if (providers_.empty() && state.HasProviders())
    {
        providers_ = state.providers();
        RebuildList();
        return;
    }

    if (providers_.empty() && !loading_)
        ReloadProviders();
}

void ProvidersTab::ReloadProviders()
{
    if (loading_)
        return;

    loading_ = true;
    status_label_->setText("Loading provider endpoints...");

    try
    {
        InputBlocker blocker;
        providers_ = client_.FetchLoginProviders();
        AppState::Instance().SetProviders(providers_);
    }
    catch (const std::exception& ex)
    {
        loading_ = false;
        status_label_->setText("Provider discovery failed.");
        ShowError("Provider Discovery Failed", ex.what());
        return;
    }

    loading_ = false;
    RebuildList();
    brls::Application::notify("Provider endpoints refreshed");
}

void ProvidersTab::RebuildList()
{
    list_container_->clearViews();

    status_label_->setText(
        "Loaded " + std::to_string(providers_.size()) + " login providers.");

    for (size_t index = 0; index < providers_.size(); ++index)
    {
        const LoginProvider& provider = providers_[index];
        auto* button                  = new brls::Button();
        button->setText(provider.display_name + "  [" + provider.code + "]");
        button->setMarginBottom(10);
        button->registerClickAction([this, index](brls::View* view) {
            return OpenProviderDialog(view, index);
        });
        list_container_->addView(button);
    }
}

bool ProvidersTab::OpenProviderDialog(brls::View* view, size_t index)
{
    if (index >= providers_.size())
        return false;

    const LoginProvider& provider = providers_[index];
    
    auto* dialog = new QrLoginDialog(provider, client_);
    brls::Application::pushActivity(new brls::Activity(dialog));
    
    return true;
}

} // namespace opennow
