#pragma once

#include <string>

namespace opennow
{

const std::string& AppHomePath();
const std::string& LegacyAppHomePath();
void PrepareAppStorage();

} // namespace opennow
