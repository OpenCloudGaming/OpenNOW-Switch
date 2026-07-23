#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace opennow::shortcut
{

namespace detail
{

inline std::uint32_t ReadU32(const std::vector<std::uint8_t>& data, size_t offset)
{
    if (offset > data.size() || data.size() - offset < 4)
        return 0;
    return static_cast<std::uint32_t>(data[offset]) |
        (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

inline std::uint64_t ReadU64(const std::vector<std::uint8_t>& data, size_t offset)
{
    if (offset > data.size() || data.size() - offset < 8)
        return 0;
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
        value = (value << 8) | data[offset + static_cast<size_t>(i)];
    return value;
}

inline void WriteU32(std::vector<std::uint8_t>& data, size_t offset, std::uint32_t value)
{
    for (size_t i = 0; i < 4; ++i)
        data[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

inline void WriteU64(std::vector<std::uint8_t>& data, size_t offset, std::uint64_t value)
{
    for (size_t i = 0; i < 8; ++i)
        data[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

inline bool CheckedRange(
    size_t base, std::uint64_t offset, std::uint64_t size, size_t total,
    size_t& begin, size_t& length)
{
    if (offset > std::numeric_limits<size_t>::max() ||
        size > std::numeric_limits<size_t>::max())
        return false;
    const size_t native_offset = static_cast<size_t>(offset);
    if (native_offset > total || base > total - native_offset)
        return false;
    begin = base + native_offset;
    length = static_cast<size_t>(size);
    return begin >= base && begin <= total && length <= total - begin;
}

inline void PatchNacpText(
    std::vector<std::uint8_t>& nacp, std::string_view title,
    std::string_view author)
{
    constexpr size_t kLanguageEntrySize = 0x300;
    constexpr size_t kNameSize = 0x200;
    constexpr size_t kAuthorSize = 0x100;
    constexpr size_t kLanguageCount = 16;
    for (size_t index = 0; index < kLanguageCount; ++index)
    {
        const size_t entry = index * kLanguageEntrySize;
        std::fill_n(nacp.begin() + static_cast<std::ptrdiff_t>(entry), kNameSize, 0);
        std::fill_n(
            nacp.begin() + static_cast<std::ptrdiff_t>(entry + kNameSize),
            kAuthorSize, 0);
        const size_t title_length = std::min(title.size(), kNameSize - 1);
        const size_t author_length = std::min(author.size(), kAuthorSize - 1);
        std::copy_n(
            reinterpret_cast<const std::uint8_t*>(title.data()), title_length,
            nacp.begin() + static_cast<std::ptrdiff_t>(entry));
        std::copy_n(
            reinterpret_cast<const std::uint8_t*>(author.data()), author_length,
            nacp.begin() + static_cast<std::ptrdiff_t>(entry + kNameSize));
    }
}

} // namespace detail

inline bool BuildShortcutNro(
    const std::vector<std::uint8_t>& template_nro,
    const std::vector<std::uint8_t>& icon_jpeg,
    std::string_view title,
    std::vector<std::uint8_t>& output)
{
    constexpr size_t kNroSizeOffset = 0x18;
    constexpr size_t kAssetHeaderSize = 0x38;
    constexpr std::uint32_t kAsetMagic = 0x54455341; // "ASET"
    constexpr size_t kMinimumNacpSize = 0x3000;

    output.clear();
    if (template_nro.size() < 0x80 || icon_jpeg.empty() || title.empty())
        return false;

    const size_t asset_offset = detail::ReadU32(template_nro, kNroSizeOffset);
    if (asset_offset < 0x80 || asset_offset > template_nro.size() ||
        template_nro.size() - asset_offset < kAssetHeaderSize ||
        detail::ReadU32(template_nro, asset_offset) != kAsetMagic)
        return false;

    size_t nacp_begin = 0;
    size_t nacp_size = 0;
    size_t romfs_begin = 0;
    size_t romfs_size = 0;
    if (!detail::CheckedRange(
            asset_offset, detail::ReadU64(template_nro, asset_offset + 0x18),
            detail::ReadU64(template_nro, asset_offset + 0x20),
            template_nro.size(), nacp_begin, nacp_size) ||
        nacp_size < kMinimumNacpSize ||
        !detail::CheckedRange(
            asset_offset, detail::ReadU64(template_nro, asset_offset + 0x28),
            detail::ReadU64(template_nro, asset_offset + 0x30),
            template_nro.size(), romfs_begin, romfs_size))
        return false;

    std::vector<std::uint8_t> nacp(
        template_nro.begin() + static_cast<std::ptrdiff_t>(nacp_begin),
        template_nro.begin() + static_cast<std::ptrdiff_t>(nacp_begin + nacp_size));
    detail::PatchNacpText(nacp, title, "OpenNOW");

    const size_t icon_offset = kAssetHeaderSize;
    const size_t new_nacp_offset = icon_offset + icon_jpeg.size();
    const size_t new_romfs_offset = new_nacp_offset + nacp.size();
    if (new_romfs_offset > std::numeric_limits<size_t>::max() - romfs_size)
        return false;

    output.reserve(asset_offset + new_romfs_offset + romfs_size);
    output.insert(output.end(), template_nro.begin(),
                  template_nro.begin() + static_cast<std::ptrdiff_t>(asset_offset));
    const size_t new_asset_begin = output.size();
    output.resize(output.size() + kAssetHeaderSize, 0);
    detail::WriteU32(output, new_asset_begin, kAsetMagic);
    detail::WriteU64(output, new_asset_begin + 0x08, icon_offset);
    detail::WriteU64(output, new_asset_begin + 0x10, icon_jpeg.size());
    detail::WriteU64(output, new_asset_begin + 0x18, new_nacp_offset);
    detail::WriteU64(output, new_asset_begin + 0x20, nacp.size());
    detail::WriteU64(output, new_asset_begin + 0x28, new_romfs_offset);
    detail::WriteU64(output, new_asset_begin + 0x30, romfs_size);
    output.insert(output.end(), icon_jpeg.begin(), icon_jpeg.end());
    output.insert(output.end(), nacp.begin(), nacp.end());
    output.insert(
        output.end(),
        template_nro.begin() + static_cast<std::ptrdiff_t>(romfs_begin),
        template_nro.begin() + static_cast<std::ptrdiff_t>(romfs_begin + romfs_size));
    return true;
}

} // namespace opennow::shortcut
