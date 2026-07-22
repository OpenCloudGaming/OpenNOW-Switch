#pragma once

#include <string_view>

namespace opennow::device_identity
{

constexpr std::string_view kLegacyPlaceholderId =
    "12345678-1234-5678-1234-567812345678";

constexpr bool IsUsableStoredDeviceId(std::string_view value)
{
    return !value.empty() && value != kLegacyPlaceholderId;
}

} // namespace opennow::device_identity
