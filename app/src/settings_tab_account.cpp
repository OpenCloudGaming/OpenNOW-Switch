#include "settings_tab.hpp"

#include "app_state.hpp"
#include "providers_tab.hpp"
#include "qr_login_dialog.hpp"
#include "ui_helpers.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

namespace opennow
{

bool SettingsTab::BeginLogin(brls::View* view)
{
    (void)view;
    try
    {
        auto& state = AppState::Instance();
        std::vector<LoginProvider> providers =
            state.HasProviders() ? state.providers() : client_.FetchLoginProviders();
        if (!state.HasProviders())
            state.SetProviders(providers);
        if (providers.empty())
            throw std::runtime_error("No GeForce NOW login providers were returned");

        const auto refresh_account = [this]() {
            RefreshSummary();
            RebuildCategory();
        };
        if (state.HasSession() && state.session()->reauthentication_required)
        {
            auto* dialog = new QrLoginDialog(
                state.session()->provider, client_, refresh_account);
            brls::Application::pushActivity(new brls::Activity(dialog));
        }
        else
        {
            auto* providers_view = new ProvidersTab(refresh_account);
            brls::Application::pushActivity(new brls::Activity(providers_view));
        }
    }
    catch (const std::exception& ex)
    {
        ShowError("GeForce NOW Login Failed", ex.what());
    }
    return true;
}

bool SettingsTab::ClearSavedLogin(brls::View* view)
{
    (void)view;

    client_.ClearSavedSession();

    auto& state = AppState::Instance();
    state.SetLibraryGames({});

    AuthSession next;
    if (client_.LoadSavedSession(next))
    {
        state.SetSession(std::move(next));
        brls::Application::notify("Active login cleared; switched to another saved account");
    }
    else
    {
        state.ClearSession();
        brls::Application::notify("Saved GeForce NOW login cleared");
    }

    RefreshSummary();
    brls::sync([this] { RebuildCategory(); });
    return true;
}

bool SettingsTab::SwitchSavedAccount(brls::View* view)
{
    (void)view;

    std::vector<AuthSession> sessions = client_.LoadSavedSessions();
    if (sessions.empty())
    {
        ShowDialog("Saved Accounts", "No saved GeForce NOW accounts are available.");
        return true;
    }

    const std::string active_user_id =
        AppState::Instance().HasSession() ? AppState::Instance().session()->user.user_id : "";

    auto* dialog = new brls::Dialog("Choose the GeForce NOW account to use.");
    for (const AuthSession& session : sessions)
    {
        const bool active = session.user.user_id == active_user_id;
        const std::string label = (active ? "Active: " : "Use: ") + session.user.display_name;
        dialog->addButton(label, [this, session]() {
            if (!client_.SetActiveSavedSession(session.user.user_id))
            {
                ShowError("Account Switch Failed", "Unable to activate the selected saved account.");
                return;
            }

            auto& state = AppState::Instance();
            state.SetSession(session);
            state.SetLibraryGames({});
            RefreshSummary();
            brls::sync([this] { RebuildCategory(); });
            brls::Application::notify("Switched to " + session.user.display_name);

            AuthSession refresh_source = session;
            refresh_source.membership_checked_at_ms = 0;
            GfnClient client = client_;
            brls::async([this, client, refresh_source = std::move(refresh_source)]() mutable {
                try
                {
                    AuthSession refreshed = client.EnsureFreshSavedSession(refresh_source);
                    brls::sync([this, refreshed = std::move(refreshed)]() mutable {
                        auto& current = AppState::Instance();
                        if (!current.HasSession() ||
                            current.session()->user.user_id != refreshed.user.user_id)
                            return;
                        current.SetSession(std::move(refreshed));
                        RebuildCategory();
                    });
                }
                catch (const std::exception&)
                {
                    // Keep the saved account active when subscription refresh is offline.
                }
            }, false);
        });
    }
    dialog->addButton("Cancel", [] {});
    dialog->open();
    return true;
}

} // namespace opennow
