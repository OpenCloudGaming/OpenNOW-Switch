#include "community_proxy_policy.hpp"

#include <cassert>
#include <string>

int main()
{
    using opennow::community_proxy::EnabledUrl;
    using opennow::community_proxy::IsCommunityProxyUrl;

    const std::string primary =
        "http://client:secret@opennow-proxy-tcp.zortos.me:3128";
    const std::string fallback =
        "http://client:secret@217.76.50.166:3128";

    assert(IsCommunityProxyUrl(primary));
    assert(IsCommunityProxyUrl(
        "client:secret@opennow-proxy-tcp.zortos.me:3128"));
    assert(IsCommunityProxyUrl("  " + fallback + "  "));
    assert(IsCommunityProxyUrl(primary + "/"));
    assert(!IsCommunityProxyUrl("https://client:secret@opennow-proxy-tcp.zortos.me:3128"));
    assert(!IsCommunityProxyUrl("http://opennow-proxy-tcp.zortos.me:3128"));
    assert(!IsCommunityProxyUrl("http://client:secret@opennow-proxy-tcp.zortos.me:8080"));
    assert(!IsCommunityProxyUrl("http://client:secret@proxy.example.com:3128"));
    assert(!IsCommunityProxyUrl(primary + "/unexpected"));
    assert(!IsCommunityProxyUrl(primary + "?redirect=example.com"));

    opennow::StreamSettings settings;
    settings.community_proxy_url = primary;
    assert(EnabledUrl(settings).empty());
    settings.community_proxy_enabled = true;
    assert(EnabledUrl(settings) == primary);
    settings.community_proxy_url =
        "client:secret@opennow-proxy-tcp.zortos.me:3128";
    assert(EnabledUrl(settings) == primary);
    settings.community_proxy_url = "http://client:secret@proxy.example.com:3128";
    assert(EnabledUrl(settings).empty());
    return 0;
}
