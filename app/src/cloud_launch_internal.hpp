#pragma once

#include "gfn_client.hpp"
#include "stream_settings.hpp"
#include <string>

namespace opennow
{
void PresentCloudStream(const SessionInfo& info, const GfnClient& client,
                        const AuthSession& auth, const std::string& title,
                        const StreamSettings& settings);
}
