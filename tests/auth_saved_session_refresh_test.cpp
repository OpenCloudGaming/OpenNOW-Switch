#include "gfn_client.hpp"
#include "gfn/internal.hpp"
#include "gfn/persistence_internal.hpp"
#include "stream_settings.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <latch>
#include <thread>

namespace
{
std::latch* refresh_started = nullptr;
std::latch* finish_refresh = nullptr;
}

namespace opennow
{
bool StreamDiagnosticsEnabled()
{
    return false;
}

StreamSettings LoadStreamSettings()
{
    return {};
}

HttpResponse HttpClient::Post(const std::string&, const std::string&,
                             const std::vector<std::string>&,
                             const std::string&, const std::string&, HttpTransferControl) const
{
    if (refresh_started)
    {
        refresh_started->count_down();
        finish_refresh->wait();
    }
    return {200, R"({"access_token":"synthetic-renewed","expires_in":3600})"};
}

HttpResponse HttpClient::Get(const std::string&, const std::string&,
                            const std::vector<std::string>&, const std::string&, HttpTransferControl) const
{
    return {200, "{}"};
}
}

int main()
{
    const auto original_directory = std::filesystem::current_path();
    std::string pattern = (std::filesystem::temp_directory_path() / "opennow-auth-XXXXXX").string();
    const char* directory = mkdtemp(pattern.data());
    assert(directory);
    const std::filesystem::path test_directory(directory);
    std::filesystem::current_path(test_directory);
    std::filesystem::create_directories(opennow::gfn::detail::GetAppHome());

    opennow::AuthSession first;
    first.user.user_id = "synthetic-account-a";
    first.tokens.access_token = "synthetic-expired";
    first.tokens.refresh_token = "synthetic-refresh";
    first.tokens.client_token = "synthetic-client";
    first.tokens.expires_at_ms = 1;
    first.tokens.client_token_expires_at_ms = opennow::gfn::detail::NowMs() + 3600000;
    first.persistence_enabled = true;
    auto second = first;
    second.user.user_id = "synthetic-account-b";

    opennow::GfnClient client;
    client.SaveSession(first);
    client.SaveSession(second);
    const auto refreshed = client.EnsureFreshSavedSession(first);
    assert(refreshed.tokens.access_token == "synthetic-renewed");
    opennow::AuthSession active;
    assert(client.LoadSavedSession(active));
    assert(active.user.user_id == second.user.user_id);
    const auto saved = client.LoadSavedSessions();
    assert(saved.size() == 2);
    assert(saved.front().tokens.access_token == "synthetic-renewed");

    client.ForceRefreshSavedSession(first);
    assert(client.LoadSavedSession(active));
    assert(active.user.user_id == second.user.user_id);

    client.ClearAllSavedSessions();
    client.EnsureFreshSavedSession(first);
    assert(client.LoadSavedSessions().empty());
    assert(!client.LoadSavedSession(active));
    client.ForceRefreshSavedSession(first);
    assert(client.LoadSavedSessions().empty());

    client.SaveSession(first);
    client.SaveSession(second);
    std::latch started(1);
    std::latch finish(1);
    refresh_started = &started;
    finish_refresh = &finish;
    std::thread worker([&] { client.ForceRefreshSavedSession(first); });
    started.wait();
    const bool account_actions_available = opennow::gfn::detail::AccountsMutex().try_lock();
    if (account_actions_available)
    {
        opennow::gfn::detail::AccountsMutex().unlock();
        auto signed_in_again = first;
        signed_in_again.tokens.access_token = "synthetic-new-login";
        client.SaveSession(signed_in_again);
        assert(client.SetActiveSavedSession(second.user.user_id));
    }
    finish.count_down();
    worker.join();
    refresh_started = nullptr;
    finish_refresh = nullptr;
    assert(account_actions_available);
    assert(client.LoadSavedSession(active));
    assert(active.user.user_id == second.user.user_id);
    assert(client.LoadSavedSessions().front().tokens.access_token == "synthetic-new-login");

    std::filesystem::current_path(original_directory);
    std::filesystem::remove_all(test_directory);
}
