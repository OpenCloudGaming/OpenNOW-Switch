#pragma once

#include <string>

namespace opennow {

class NetworkUtils {
public:
    static std::string GetLocalIPAddress();
    static bool HasInternetConnection();
};

} // namespace opennow
