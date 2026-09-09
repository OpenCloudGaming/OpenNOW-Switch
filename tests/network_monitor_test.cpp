#include "network_monitor.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

template <typename Predicate>
void WaitUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

void TestInitialSnapshot() {
    opennow::NetworkMonitor monitor;
    const auto snapshot = monitor.snapshot();
    assert(snapshot.internet_connected);
    assert(snapshot.connection_info.connected);
    assert(snapshot.connection_info.type == opennow::NetworkConnectionType::Unknown);
    assert(snapshot.connection_info.wifi_band == opennow::network::WifiBand::Unknown);
    assert(snapshot.connection_info.wifi_strength == -1);
    monitor.stop();
    monitor.stop();
}

void TestPollingAndNonfatalUnknownResults() {
    std::atomic<int> connection_calls {0};
    std::atomic<int> info_calls {0};
    std::atomic<bool> connected {false};
    std::atomic<bool> unknown {false};
    opennow::NetworkMonitor monitor(
        [&] {
            connection_calls.fetch_add(1);
            return connected.load();
        },
        [&] {
            info_calls.fetch_add(1);
            opennow::NetworkConnectionInfo info;
            if (!unknown.load()) {
                info.type = opennow::NetworkConnectionType::Wifi;
                info.wifi_band = opennow::network::WifiBand::Ghz5;
                info.wifi_strength = 3;
            }
            return info;
        },
        5ms, 20ms);
    monitor.start();
    monitor.start();
    WaitUntil([&] {
        const auto snapshot = monitor.snapshot();
        return !snapshot.internet_connected &&
            snapshot.connection_info.wifi_band == opennow::network::WifiBand::Ghz5;
    });
    connected.store(true);
    unknown.store(true);
    WaitUntil([&] {
        const auto snapshot = monitor.snapshot();
        return snapshot.internet_connected && snapshot.connection_info.connected &&
            snapshot.connection_info.type == opennow::NetworkConnectionType::Unknown;
    });
    monitor.stop();
    assert(connection_calls.load() >= 2);
    assert(info_calls.load() >= 2);
    connected.store(false);
    monitor.start();
    WaitUntil([&] { return !monitor.snapshot().internet_connected; });
    monitor.stop();
}

void TestInflightQueryDoesNotBlockSnapshotOrOutliveStop() {
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future();
    std::atomic<int> connection_calls {0};
    std::atomic<int> info_calls {0};
    opennow::NetworkMonitor monitor(
        [&] {
            connection_calls.fetch_add(1);
            entered.set_value();
            release_future.wait();
            return false;
        },
        [&] {
            info_calls.fetch_add(1);
            return opennow::NetworkConnectionInfo {};
        });
    monitor.start();
    assert(entered_future.wait_for(2s) == std::future_status::ready);
    auto read_snapshot = std::async(std::launch::async, [&] { return monitor.snapshot(); });
    assert(read_snapshot.wait_for(2s) == std::future_status::ready);
    assert(read_snapshot.get().internet_connected);
    monitor.request_stop();
    release.set_value();
    monitor.stop();
    assert(connection_calls.load() == 1);
    assert(info_calls.load() == 0);
    assert(monitor.snapshot().internet_connected);
}

void TestIdleWaitIsInterruptible() {
    opennow::NetworkMonitor monitor(
        [] { return true; },
        [] {
            opennow::NetworkConnectionInfo info;
            info.type = opennow::NetworkConnectionType::Ethernet;
            return info;
        },
        60s, 60s);
    monitor.start();
    WaitUntil([&] {
        return monitor.snapshot().connection_info.type ==
            opennow::NetworkConnectionType::Ethernet;
    });
    auto stopped = std::async(std::launch::async, [&] { monitor.stop(); });
    assert(stopped.wait_for(2s) == std::future_status::ready);
    stopped.get();
}

void TestDestructorJoinsInflightQuery() {
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future();
    auto monitor = std::make_unique<opennow::NetworkMonitor>([&] {
        entered.set_value();
        release_future.wait();
        return true;
    });
    monitor->start();
    assert(entered_future.wait_for(2s) == std::future_status::ready);
    monitor->request_stop();
    auto destroyed = std::async(std::launch::async, [&] { monitor.reset(); });
    assert(destroyed.wait_for(10ms) == std::future_status::timeout);
    release.set_value();
    assert(destroyed.wait_for(2s) == std::future_status::ready);
    destroyed.get();
}

int main() {
    TestInitialSnapshot();
    TestPollingAndNonfatalUnknownResults();
    TestInflightQueryDoesNotBlockSnapshotOrOutliveStop();
    TestIdleWaitIsInterruptible();
    TestDestructorJoinsInflightQuery();
}
