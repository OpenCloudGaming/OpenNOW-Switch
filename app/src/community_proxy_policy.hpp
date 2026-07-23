#pragma once

#include "stream_settings.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace opennow::community_proxy
{

inline constexpr std::string_view kHost = "opennow-proxy-tcp.zortos.me";
inline constexpr std::string_view kFallbackHost = "217.76.50.166";
inline constexpr std::string_view kPort = "3128";
inline constexpr std::string_view kProvisionUrl =
    "https://opennow-proxy.zortos.me/api/public/proxy";

inline std::string Trim(std::string_view value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    return first < last ? std::string(first, last) : std::string();
}

inline std::string NormalizeUrl(std::string_view raw)
{
    std::string value = Trim(raw);
    if (!value.empty() && value.find("://") == std::string::npos)
        value.insert(0, "http://");
    return value;
}

inline bool IsCommunityProxyUrl(std::string_view raw)
{
    const std::string value = NormalizeUrl(raw);
    constexpr std::string_view scheme = "http://";
    if (value.size() <= scheme.size() ||
        !std::equal(scheme.begin(), scheme.end(), value.begin(), [](char left, char right) {
            return std::tolower(static_cast<unsigned char>(left)) ==
                   std::tolower(static_cast<unsigned char>(right));
        }))
        return false;

    const size_t authority_end = value.find_first_of("/?#", scheme.size());
    if (authority_end != std::string::npos &&
        !(authority_end + 1 == value.size() && value[authority_end] == '/'))
        return false;

    const std::string authority = value.substr(
        scheme.size(),
        authority_end == std::string::npos ? std::string::npos : authority_end - scheme.size());
    const size_t at = authority.rfind('@');
    if (at == std::string::npos || at == 0 || at + 1 >= authority.size())
        return false;

    std::string endpoint = authority.substr(at + 1);
    std::transform(endpoint.begin(), endpoint.end(), endpoint.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return endpoint == std::string(kHost) + ":" + std::string(kPort) ||
           endpoint == std::string(kFallbackHost) + ":" + std::string(kPort);
}

inline std::string RuntimeUrl(std::string_view raw)
{
    std::string value = NormalizeUrl(raw);
    if (!IsCommunityProxyUrl(value))
        return {};

    constexpr std::string_view scheme = "http://";
    const size_t authority_end = value.find_first_of("/?#", scheme.size());
    const size_t authority_size =
        authority_end == std::string::npos ? value.size() - scheme.size()
                                           : authority_end - scheme.size();
    const size_t at = value.rfind('@', scheme.size() + authority_size);
    const size_t endpoint_begin = at + 1;
    const size_t endpoint_size = scheme.size() + authority_size - endpoint_begin;
    std::string endpoint = value.substr(endpoint_begin, endpoint_size);
    std::transform(endpoint.begin(), endpoint.end(), endpoint.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    // The public hostname is proxied through Cloudflare, which does not expose
    // the authenticated HTTP proxy's TCP port. Keep credentials intact while
    // using the service's pinned direct endpoint so the console also avoids a
    // DNS dependency when starting or polling a session.
    if (endpoint == std::string(kHost) + ":" + std::string(kPort))
    {
        value.replace(
            endpoint_begin,
            endpoint_size,
            std::string(kFallbackHost) + ":" + std::string(kPort));
    }
    return value;
}

inline std::string EnabledUrl(const StreamSettings& settings)
{
    if (!settings.community_proxy_enabled || !IsCommunityProxyUrl(settings.community_proxy_url))
        return {};
    return RuntimeUrl(settings.community_proxy_url);
}

} // namespace opennow::community_proxy
