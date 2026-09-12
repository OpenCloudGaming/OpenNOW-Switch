#include "app_state.hpp"

#include <cassert>

int main()
{
    auto& state = opennow::AppState::Instance();
    assert(!state.IsCurrentSession(state.session_generation()));

    opennow::AuthSession account;
    account.user.user_id = "account-a";
    account.provider.idp_id = "provider-a";
    account.provider.code = "NVIDIA";
    account.provider.streaming_service_url = "https://example.invalid/";
    state.SetSession(account);
    const auto first = state.session_generation();
    assert(state.IsCurrentSession(first));

    account.last_refresh_at_ms = 100;
    account.membership_checked_at_ms = 200;
    account.user.membership_tier = "ULTIMATE";
    account.tokens.expires_at_ms = 300;
    state.SetSession(account);
    assert(state.IsCurrentSession(first));

    auto other = account;
    other.user.user_id = "account-b";
    state.SetSession(other);
    assert(!state.IsCurrentSession(first));
    const auto second = state.session_generation();
    state.SetSession(account);
    assert(!state.IsCurrentSession(first));
    assert(!state.IsCurrentSession(second));

    for (int field = 0; field < 3; ++field)
    {
        const auto before = state.session_generation();
        if (field == 0)
            account.provider.idp_id = "provider-b";
        else if (field == 1)
            account.provider.code = "ALLIANCE";
        else
            account.provider.streaming_service_url = "https://alliance.invalid/";
        state.SetSession(account);
        assert(!state.IsCurrentSession(before));
    }

    const auto before_logout = state.session_generation();
    state.ClearSession();
    assert(!state.IsCurrentSession(before_logout));
    assert(!state.IsCurrentSession(state.session_generation()));
    state.SetSession(account);
    assert(!state.IsCurrentSession(before_logout));
    assert(state.IsCurrentSession(state.session_generation()));

    const auto before_login = state.session_generation();
    state.ActivateSession(account);
    assert(!state.IsCurrentSession(before_login));
    const auto activated = state.session_generation();
    state.SetSession(account);
    assert(state.IsCurrentSession(activated));
}
