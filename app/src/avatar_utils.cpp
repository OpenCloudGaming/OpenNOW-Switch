#include "avatar_utils.hpp"

extern "C"
{
#include "utils.h"
}

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace opennow
{
namespace
{

std::string NormalizeEmail(std::string email)
{
    const auto first = std::find_if_not(email.begin(), email.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(email.rbegin(), email.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last)
        return {};

    email = std::string(first, last);
    std::transform(email.begin(), email.end(), email.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return email;
}

std::string Md5Hex(const std::string& value)
{
    unsigned char digest[16] {};
    utils_get_md5(value.data(), value.size(), digest);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : digest)
        output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}

} // namespace

std::string ResolveAvatarUrl(const AuthUser& user)
{
    return ResolveAvatarUrl(user.email, user.avatar_url);
}

std::string ResolveAvatarUrl(const std::string& email, const std::string& picture_url)
{
    if (!picture_url.empty())
        return picture_url;

    const std::string normalized = NormalizeEmail(email);
    if (normalized.empty())
        return {};

    return "https://www.gravatar.com/avatar/" + Md5Hex(normalized) +
           "?s=160&d=identicon&r=g";
}

} // namespace opennow
