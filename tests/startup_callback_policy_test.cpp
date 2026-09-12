#include "startup_callback_policy.hpp"

#include <cassert>
#include <functional>
#include <vector>

namespace
{

struct StartupView
{
    std::shared_ptr<std::atomic_bool> alive = std::make_shared<std::atomic_bool>(true);
    int updates = 0;

    ~StartupView()
    {
        alive->store(false);
    }
};

} // namespace

int main()
{
    auto view = std::make_unique<StartupView>();
    auto* target = view.get();
    const auto alive = view->alive;
    int applied = 0;
    std::vector<std::function<void()>> queue;
    const auto post_update = [target, alive, &queue, &applied] {
        queue.push_back(opennow::GuardStartupCallback(alive, [target, &applied] {
            ++target->updates;
            ++applied;
        }));
    };

    post_update();
    queue.back()();
    assert(view->updates == 1 && applied == 1);
    queue.clear();

    post_update();
    view.reset();
    queue.back()();
    assert(applied == 1);
    queue.clear();

    post_update();
    queue.back()();
    assert(applied == 1);

    auto other = std::make_unique<StartupView>();
    auto update_other = opennow::GuardStartupCallback(other->alive, [target = other.get()] {
        ++target->updates;
    });
    update_other();
    assert(other->updates == 1);
    other.reset();
    update_other();
}
