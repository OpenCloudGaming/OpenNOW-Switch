#pragma once

#include "internal.hpp"

namespace opennow::gfn::detail
{

std::recursive_mutex& AccountsMutex();
std::vector<AuthSession> LoadAccountsFromDisk(std::string* active_user_id = nullptr);
void SaveAccountsToDisk(
    const std::vector<AuthSession>& sessions, const std::string& active_user_id);

} // namespace opennow::gfn::detail
