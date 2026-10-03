#include "cloud_launch_internal.hpp"
#include "StreamView.hpp"
#include <borealis.hpp>

namespace opennow
{
void PresentCloudStream(const SessionInfo& info, const GfnClient& client,
                        const AuthSession& auth, const std::string& title,
                        const StreamSettings& settings)
{
    const std::string& token = auth.tokens.id_token.empty()
        ? auth.tokens.access_token : auth.tokens.id_token;
    brls::Application::pushActivity(new brls::Activity(StreamView::create(
        info.signaling_url, token, info.session_id, info.media_ip, info.media_port,
        info.ice_servers, client, auth, title, settings)));
}
}
