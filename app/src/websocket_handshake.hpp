#pragma once

#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace opennow::websocket
{

inline std::optional<std::string> AcceptForKey(std::string_view key)
{
    const std::string challenge = std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<unsigned char, 20> digest {};
    if (mbedtls_sha1(reinterpret_cast<const unsigned char*>(challenge.data()),
                     challenge.size(), digest.data()) != 0)
        return std::nullopt;
    std::array<unsigned char, 29> encoded {};
    size_t size = 0;
    if (mbedtls_base64_encode(encoded.data(), encoded.size(), &size,
                             digest.data(), digest.size()) != 0)
        return std::nullopt;
    return std::string(reinterpret_cast<const char*>(encoded.data()), size);
}

inline std::string_view TrimHeaderWhitespace(std::string_view value)
{
    const size_t first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return {};
    return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

inline bool HeaderEquals(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;
    for (size_t i = 0; i < left.size(); ++i) {
        const char a = left[i] >= 'A' && left[i] <= 'Z' ? left[i] + ('a' - 'A') : left[i];
        const char b = right[i] >= 'A' && right[i] <= 'Z' ? right[i] + ('a' - 'A') : right[i];
        if (a != b)
            return false;
    }
    return true;
}

inline bool HasHeaderToken(std::string_view value, std::string_view token)
{
    while (!value.empty()) {
        const size_t comma = value.find(',');
        if (HeaderEquals(TrimHeaderWhitespace(value.substr(0, comma)), token))
            return true;
        if (comma == std::string_view::npos)
            break;
        value.remove_prefix(comma + 1);
    }
    return false;
}

inline bool ValidateUpgrade(std::string_view response, std::string_view expected_accept)
{
    const size_t status_end = response.find("\r\n");
    if (status_end == std::string_view::npos)
        return false;
    const auto status = response.substr(0, status_end);
    if (status != "HTTP/1.1 101" && !status.starts_with("HTTP/1.1 101 "))
        return false;

    bool upgrade = false;
    bool connection = false;
    bool accept = false;
    size_t start = status_end + 2;
    while (start < response.size()) {
        const size_t end = response.find("\r\n", start);
        if (end == std::string_view::npos)
            return false;
        const auto line = response.substr(start, end - start);
        if (line.empty())
            return upgrade && connection && accept;
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0 ||
            line.front() == ' ' || line.front() == '\t')
            return false;
        const auto name = line.substr(0, colon);
        const auto value = TrimHeaderWhitespace(line.substr(colon + 1));
        if (HeaderEquals(name, "Upgrade")) {
            upgrade = upgrade || HasHeaderToken(value, "websocket");
        } else if (HeaderEquals(name, "Connection")) {
            connection = connection || HasHeaderToken(value, "Upgrade");
        } else if (HeaderEquals(name, "Sec-WebSocket-Accept")) {
            if (accept || value != expected_accept || value.empty())
                return false;
            accept = true;
        }
        start = end + 2;
    }
    return false;
}

}
