#include "http_client.hpp"

#include <cassert>
#include <chrono>
#include <stdexcept>
#include <thread>

int main(int argc, char** argv)
{
    assert(argc == 3);
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    const std::string http_url = argv[1];
    const std::string https_url = argv[2];
    opennow::HttpClient client;
    const auto response = client.Get(http_url + "/small", "integration-test");
    assert(response.status_code == 200 && response.body == "test");

    bool oversized = false;
    try
    {
        client.Get(http_url + "/large", "integration-test", {}, {}, {.max_body_bytes = 32});
    }
    catch (const std::runtime_error&)
    {
        oversized = true;
    }
    assert(oversized);

    std::atomic_bool cancelled = false;
    std::jthread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cancelled.store(true);
    });
    const auto start = std::chrono::steady_clock::now();
    bool stopped = false;
    try
    {
        client.Get(http_url + "/slow", "integration-test", {}, {}, {.cancelled = &cancelled});
    }
    catch (const std::runtime_error&)
    {
        stopped = true;
    }
    assert(stopped && cancelled.load());
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));

    bool untrusted = false;
    try
    {
        client.Get(https_url + "/small?private-query", "integration-test");
    }
    catch (const std::runtime_error& error)
    {
        untrusted = true;
        const std::string message = error.what();
        assert(message.find("TLS backend:") != std::string::npos);
        assert(message.find("private-query") == std::string::npos);
    }
    assert(untrusted);
    curl_global_cleanup();
}
