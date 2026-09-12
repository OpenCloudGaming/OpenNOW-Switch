#include "gfn_client.hpp"
#include "gfn/internal.hpp"
#include "stream_settings.hpp"

#include <cassert>
#include <stdexcept>

namespace
{
int client_refreshes = 0;
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

HttpResponse HttpClient::Post(const std::string& url, const std::string&,
                             const std::vector<std::string>&,
                             const std::string& body, const std::string&, HttpTransferControl) const
{
    assert(url == "https://login.nvidia.com/token");
    assert(body.find("client_token=synthetic-client") != std::string::npos);
    ++client_refreshes;
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
    opennow::AuthSession session;
    session.user.user_id = "synthetic-user";
    session.tokens.access_token = "synthetic-expired";
    session.tokens.expires_at_ms = 1;
    session.tokens.client_token = "synthetic-client";
    session.tokens.client_token_expires_at_ms = opennow::gfn::detail::NowMs() + 3600000;
    const auto refreshed = opennow::GfnClient().EnsureFreshSession(session);
    assert(client_refreshes == 1);
    assert(refreshed.tokens.access_token == "synthetic-renewed");
    assert(refreshed.tokens.client_token == "synthetic-client");
    assert(refreshed.tokens.expires_at_ms > opennow::gfn::detail::NowMs());

    session.tokens.client_token.clear();
    bool requires_login = false;
    try
    {
        opennow::GfnClient().EnsureFreshSession(session);
    }
    catch (const opennow::ReauthenticationRequired&)
    {
        requires_login = true;
    }
    assert(requires_login);
    assert(client_refreshes == 1);
}
