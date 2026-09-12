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
        const auto refresh_account = [this, alive = alive_]() {
            if (!alive->load())
                return;
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
        state.ActivateSession(std::move(next));
        brls::Application::notify("Active login cleared; switched to another saved account");
    }
    else
    {
        state.ClearSession();
        brls::Application::notify("Saved GeForce NOW login cleared");
    }

    RefreshSummary();
    brls::sync([this, alive = alive_] {
        if (alive->load())
            RebuildCategory();
    });
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
        dialog->addButton(label, [this, alive = alive_, session]() {
            if (!alive->load())
                return;
            if (!client_.SetActiveSavedSession(session.user.user_id))
            {
                ShowError("Account Switch Failed", "Unable to activate the selected saved account.");
                return;
            }

            auto& state = AppState::Instance();
            state.ActivateSession(session);
            state.SetLibraryGames({});
            RefreshSummary();
            brls::sync([this, alive] {
                if (alive->load())
                    RebuildCategory();
            });
            brls::Application::notify("Switched to " + session.user.display_name);

            AuthSession refresh_source = session;
            refresh_source.membership_checked_at_ms = 0;
            GfnClient client = client_;
            const auto generation = state.session_generation();
            brls::async([this, alive, generation, client, refresh_source = std::move(refresh_source)]() mutable {
                if (!alive->load())
                    return;
                try
                {
                    AuthSession refreshed = client.EnsureFreshSavedSession(refresh_source);
                    brls::sync([this, alive, generation, refreshed = std::move(refreshed)]() mutable {
                        if (!alive->load())
                            return;
                        auto& current = AppState::Instance();
                        if (!current.IsCurrentSession(generation))
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
