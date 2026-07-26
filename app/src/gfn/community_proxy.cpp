#include "internal.hpp"

#include "../app_version.hpp"
#include "../community_proxy_policy.hpp"
#include "../device_identity_policy.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace opennow
{
using namespace gfn::detail;

namespace
{

std::string CommunityProxyClientId()
{
    const std::string path = GetAppHome() + "/community_proxy_client_id.txt";
    const std::string stored = Trim(ReadTextFile(path));
    if (device_identity::IsCanonicalUuid(stored))
        return stored;

    const std::string device_id = GenerateDeviceId();
    if (device_identity::IsCanonicalUuid(device_id))
        return device_id;

    const std::string generated = GenerateUuid();
    WriteTextFileAtomically(path, generated);
    return generated;
}

} // namespace

std::string GfnClient::ProvisionCommunityProxy() const
{
    JsonPtr request(json_object(), &json_decref);
    json_object_set_new(request.get(), "clientId", json_string(CommunityProxyClientId().c_str()));
    const std::string body = DumpJson(request.get());

    const HttpResponse response = http_client_.Post(
        std::string(community_proxy::kProvisionUrl),
        "OpenNOW-Switch/" + std::string(kAppVersion),
        {"Accept: application/json", "Content-Type: application/json"},
        body);

    JsonPtr payload(nullptr, &json_decref);
    try
    {
        payload = LoadJson(response.body);
    }
    catch (const std::exception&)
    {
        if (response.status_code >= 200 && response.status_code < 300)
            throw std::runtime_error("Community proxy returned an invalid response");
    }

    if (response.status_code < 200 || response.status_code >= 300)
    {
        const std::string detail = payload ? GetString(payload.get(), "message") : "";
        throw std::runtime_error(
            detail.empty()
                ? "Community proxy activation failed with HTTP " +
                    std::to_string(response.status_code)
                : detail);
    }

    std::string proxy_url = GetString(payload.get(), "proxyUrl");
    if (proxy_url.empty())
    {
        const std::string username = GetString(payload.get(), "username");
        const std::string password = GetString(payload.get(), "password");
        if (!username.empty() && !password.empty())
        {
            proxy_url = "http://" + UrlEncode(username) + ":" + UrlEncode(password) + "@" +
                std::string(community_proxy::kHost) + ":" +
                std::string(community_proxy::kPort);
        }
    }

    proxy_url = community_proxy::NormalizeUrl(proxy_url);
    if (!community_proxy::IsCommunityProxyUrl(proxy_url))
        throw std::runtime_error("Community proxy returned an invalid endpoint");
    return proxy_url;
}

} // namespace opennow
