#pragma once

#include <string>

namespace opennow
{

void ResetNteAutoLoginLog(const std::string& session_context);
void AppendNteAutoLoginLog(const std::string& line);

} // namespace opennow
