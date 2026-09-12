#include "cover_image_worker.hpp"

#include <cassert>
#include <future>
#include <stdexcept>
#include <vector>

int main()
{
    opennow::CoverImageWorker worker;
    std::promise<void> started;
    std::promise<void> release;
    const auto release_signal = release.get_future().share();
    auto active = std::make_shared<std::atomic_bool>(false);
    assert(worker.TrySubmit(active, [&] {
        started.set_value();
        release_signal.wait();
    }));
    started.get_future().wait();

    std::vector<int> executed;
    std::vector<std::shared_ptr<std::atomic_bool>> tokens;
    std::promise<void> finished;
    for (int index = 0; index < 60; ++index)
    {
        auto token = std::make_shared<std::atomic_bool>(false);
        tokens.push_back(token);
        const bool accepted = worker.TrySubmit(token, [&, index] {
            executed.push_back(index);
        });
        assert(accepted == (index < 30));
    }
    for (const auto& token : tokens)
        assert(!token->load());
    tokens[0]->store(true);
    assert(worker.TrySubmit(std::make_shared<std::atomic_bool>(false), [&] {
        executed.push_back(60);
        finished.set_value();
    }));
    release.set_value();
    finished.get_future().wait();
    assert(executed.size() == 30);
    assert(executed.front() == 1 && executed.back() == 60);
    std::promise<void> retried;
    for (int index = 30; index < 60; ++index)
    {
        assert(worker.TrySubmit(tokens[index], [&, index] {
            executed.push_back(index);
            if (index == 59)
                retried.set_value();
        }));
    }
    retried.get_future().wait();
    assert(executed.size() == 60);
    auto failed = std::make_shared<std::atomic_bool>(false);
    assert(worker.TrySubmit(failed, [] { throw std::runtime_error("transfer failed"); }));
    std::promise<void> after_failure;
    assert(worker.TrySubmit(std::make_shared<std::atomic_bool>(false), [&] {
        after_failure.set_value();
    }));
    after_failure.get_future().wait();
    assert(!failed->load());
    worker.Stop();
    auto after_stop = std::make_shared<std::atomic_bool>(false);
    assert(!worker.TrySubmit(after_stop, [] { assert(false); }));
    assert(!after_stop->load());
    worker.Stop();

    opennow::CoverImageWorker stopping;
    std::promise<void> second_started;
    std::promise<void> second_release;
    auto second_signal = second_release.get_future().share();
    auto second_active = std::make_shared<std::atomic_bool>(false);
    assert(stopping.TrySubmit(second_active, [&] {
        second_started.set_value();
        second_signal.wait();
        assert(second_active->load());
    }));
    second_started.get_future().wait();
    auto pending = std::make_shared<std::atomic_bool>(false);
    assert(stopping.TrySubmit(pending, [] { assert(false); }));
    auto stopped = std::async(std::launch::async, [&] { stopping.Stop(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!second_active->load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(second_active->load());
    second_release.set_value();
    stopped.get();
    assert(pending->load());
}
