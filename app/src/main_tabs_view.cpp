#include "main_tabs_view.hpp"

#include "app_state.hpp"
#include "catalog_tab.hpp"
#include "gfn_client.hpp"
#include "library_tab.hpp"
#include "settings_tab.hpp"
#include "localization.hpp"

#include <exception>
#include <chrono>
#include <utility>

namespace opennow
{

MainTabsView::MainTabsView()
{
    auto& state = AppState::Instance();
    if (!state.IsSessionLoaded())
    {
        AuthSession session;
        GfnClient client;
        if (client.LoadSavedSession(session))
        {
            try
            {
                session = client.EnsureFreshSavedSession(session);
            }
            catch (const ReauthenticationRequired&)
            {
                session.reauthentication_required = true;
                brls::Application::notify(
                    "Saved session expired; reconnect the account from Settings");
            }
            catch (const std::exception&)
            {
                // Keep the saved account during temporary network failures.
                brls::Application::notify("Saved account loaded; automatic renewal will retry when online");
            }
            state.SetSession(std::move(session));
        }
        else
            state.MarkSessionLoaded();
    }
    addTab(Tr("Store"), []() { return new CatalogTab(); });
    addTab(Tr("Library"), []() { return new LibraryTab(); });
    addTab(Tr("Settings"), []() { return new SettingsTab(); });
    focusTab(1);
    last_auth_check_ = std::chrono::steady_clock::now();
}

void MainTabsView::MaybeRefreshAuthentication()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - last_auth_check_ < std::chrono::minutes(1))
        return;
    last_auth_check_ = now;

    auto& state = AppState::Instance();
    if (!state.HasSession())
        return;

    const AuthSession session = *state.session();
    if (session.reauthentication_required)
        return;

    if (auth_refresh_running_->exchange(true))
        return;
    const auto guard = auth_refresh_running_;
    brls::async([session, guard]() mutable {
        try
        {
            GfnClient client;
            AuthSession refreshed = client.RecoverSavedSession(session);
            brls::sync([refreshed = std::move(refreshed), guard]() mutable {
                auto& current = AppState::Instance();
                if (current.HasSession() &&
                    current.session()->user.user_id == refreshed.user.user_id)
                {
                    current.SetSession(std::move(refreshed));
                }
                guard->store(false);
            });
        }
        catch (const ReauthenticationRequired&)
        {
            brls::sync([session, guard]() mutable {
                auto& current = AppState::Instance();
                if (current.HasSession() && current.session()->user.user_id == session.user.user_id)
                {
                    AuthSession invalid = *current.session();
                    invalid.reauthentication_required = true;
                    current.SetSession(std::move(invalid));
                    brls::Application::notify(
                        "Saved session expired; reconnect the account from Settings");
                }
                guard->store(false);
            });
        }
        catch (const std::exception&)
        {
            brls::sync([guard]() {
                guard->store(false);
            });
        }
    });
}

void MainTabsView::draw(NVGcontext* vg, float x, float y, float width, float height,
                        brls::Style style, brls::FrameContext* ctx)
{
    MaybeRefreshAuthentication();
    TopBarFrame::draw(vg, x, y, width, height, style, ctx);
}

} // namespace opennow
