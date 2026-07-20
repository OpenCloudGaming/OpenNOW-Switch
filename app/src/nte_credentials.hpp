#pragma once

#include <string>

namespace opennow
{

struct NteCredentials
{
    std::string email;
    std::string password;

    bool valid() const;
};

bool IsNevernessToEverness(const std::string& title);
NteCredentials ParseNteCredentials(const std::string& text);
std::string SerializeNteCredentials(const NteCredentials& credentials);
NteCredentials LoadNteCredentials();
bool SaveNteCredentials(const NteCredentials& credentials);
bool ClearNteCredentials();
std::string NteCredentialsPath();

} // namespace opennow
