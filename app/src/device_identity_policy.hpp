#pragma once

#include <cstddef>
#include <string_view>

namespace opennow::device_identity
{

constexpr std::string_view kLegacyPlaceholderId =
    "12345678-1234-5678-1234-567812345678";

constexpr bool IsHexDigit(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

constexpr bool IsCanonicalUuid(std::string_view value)
{
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-')
        return false;

    for (std::size_t index = 0; index < value.size(); ++index)
    {
        if (index == 8 || index == 13 || index == 18 || index == 23)
            continue;
        if (!IsHexDigit(value[index]))
            return false;
    }
    return true;
}

constexpr bool IsUsableStoredDeviceId(std::string_view value)
{
    return !value.empty() && value != kLegacyPlaceholderId;
}

} // namespace opennow::device_identity
