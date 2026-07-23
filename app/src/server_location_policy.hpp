#pragma once

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>

namespace opennow::server_location
{

inline bool IsAutomatic(const std::string& value)
{
    return value.empty() || value == "Auto";
}

inline bool IsValidStreamingBaseUrl(const std::string& value)
{
    static constexpr const char* kHttpsPrefix = "https://";
    if (value.rfind(kHttpsPrefix, 0) != 0 || value.size() <= 8)
        return false;

    const size_t authority_end = value.find('/', 8);
    const std::string authority = value.substr(8, authority_end - 8);
    if (authority.empty() || authority.find('@') != std::string::npos)
        return false;

    return std::none_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) || ch < 0x20 || ch == 0x7f;
    });
}

inline std::string NormalizeStreamingBaseUrl(std::string value)
{
    if (!IsValidStreamingBaseUrl(value))
        return {};
    if (value.back() != '/')
        value.push_back('/');
    return value;
}

template <typename RegionRange>
std::string SelectBestMeasuredRegionUrl(const RegionRange& regions)
{
    int best_ping_ms = std::numeric_limits<int>::max();
    std::string best_url;
    for (const auto& region : regions)
    {
        if (region.ping_ms < 0 || region.ping_ms >= best_ping_ms)
            continue;

        const std::string normalized = NormalizeStreamingBaseUrl(region.url);
        if (normalized.empty())
            continue;

        best_ping_ms = region.ping_ms;
        best_url = normalized;
    }
    return best_url;
}

inline std::string ResolveStreamingBaseUrl(
    const std::string& selected_region,
    const std::string& provider_url,
    const std::string& automatic_region_url = {})
{
    if (!IsAutomatic(selected_region))
    {
        const std::string selected = NormalizeStreamingBaseUrl(selected_region);
        if (!selected.empty())
            return selected;
    }
    else
    {
        const std::string automatic =
            NormalizeStreamingBaseUrl(automatic_region_url);
        if (!automatic.empty())
            return automatic;
    }

    return NormalizeStreamingBaseUrl(provider_url);
}

} // namespace opennow::server_location
