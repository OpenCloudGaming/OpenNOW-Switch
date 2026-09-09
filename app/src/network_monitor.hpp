#pragma once

#include "network_utils.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace opennow {

struct NetworkMonitorSnapshot {
    bool internet_connected = true;
    NetworkConnectionInfo connection_info {};
};

class NetworkMonitor {
public:
    NetworkMonitor(
        std::function<bool()> connection_query = NetworkUtils::HasInternetConnection,
        std::function<NetworkConnectionInfo()> info_query = NetworkUtils::GetConnectionInfo,
        std::chrono::milliseconds connection_interval = std::chrono::seconds(1),
        std::chrono::milliseconds info_interval = std::chrono::seconds(30));
    ~NetworkMonitor();

    void start();
    void request_stop();
    void stop();
    NetworkMonitorSnapshot snapshot() const;

private:
    void run();

    std::function<bool()> connection_query_;
    std::function<NetworkConnectionInfo()> info_query_;
    std::chrono::milliseconds connection_interval_;
    std::chrono::milliseconds info_interval_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    NetworkMonitorSnapshot snapshot_ {};
    bool stop_requested_ = true;
    std::thread worker_;
};

} // namespace opennow
