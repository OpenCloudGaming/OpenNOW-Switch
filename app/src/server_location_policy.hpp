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

inline std::string CompactServerLabel(std::string value, size_t max_length = 15)
{
    const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    if (value.empty())
        return "--";

    const size_t scheme = value.find("://");
    if (scheme != std::string::npos)
        value.erase(0, scheme + 3);

    const size_t authority_end = value.find_first_of("/?#");
    if (authority_end != std::string::npos)
        value.resize(authority_end);
    const size_t user_info = value.rfind('@');
    if (user_info != std::string::npos)
        value.erase(0, user_info + 1);

    if (!value.empty() && value.front() == '[')
    {
        const size_t bracket = value.find(']');
        if (bracket != std::string::npos)
            value = value.substr(1, bracket - 1);
    }
    else
    {
        const size_t colon = value.rfind(':');
        if (colon != std::string::npos && value.find(':') == colon)
            value.resize(colon);
    }

    while (!value.empty() && value.back() == '.')
        value.pop_back();

    const bool ipv4 = !value.empty() &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0 || ch == '.';
        });
    if (!ipv4)
    {
        const size_t first_dot = value.find('.');
        if (first_dot != std::string::npos)
            value.resize(first_dot);
    }

    std::string label;
    label.reserve(value.size());
    for (unsigned char ch : value)
    {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '.' || ch == ':')
            label.push_back(static_cast<char>(std::toupper(ch)));
    }

    if (label.empty())
        return "--";
    if (max_length == 0)
        return {};
    if (label.size() > max_length)
    {
        label.resize(max_length);
        label.back() = '~';
    }
    return label;
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
