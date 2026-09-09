#include "controller_assignment_policy.hpp"

#include <array>
#include <cassert>
#include <cstdint>

int main()
{
    using opennow::input::ControllerAssignments;
    using opennow::input::ControllerBitmap;

    ControllerAssignments docked;
    assert(docked.Assign(1) == 0);
    assert(docked.Assign(2) == 1);
    assert(docked.Assign(3) == 2);
    assert(docked.Assign(4) == 3);
    assert(docked.Assign(1) == 0);

    ControllerAssignments handheld;
    assert(handheld.Assign(0) == 0);
    assert(handheld.Assign(1) == 1);
    assert(handheld.Assign(2) == 2);
    assert(handheld.Assign(3) == 3);
    assert(handheld.Assign(4) == -1);

    assert(handheld.ControllerForSource(2) == 2);
    assert(handheld.Assign(2) == 2);

    ControllerAssignments sparse;
    assert(sparse.Assign(4) == 0);
    sparse.Release(4);
    assert(sparse.ControllerForSource(4) == -1);
    assert(sparse.Assign(3) == 0);
    assert(sparse.Assign(4) == 1);
    sparse.Release(3);
    assert(sparse.ControllerForSource(4) == 1);
    assert(sparse.Assign(2) == 0);
    assert(sparse.Assign(4) == 1);
    sparse.Release(3);
    sparse.Release(100);
    assert(sparse.Assign(100) == -1);
    assert(sparse.ControllerForSource(100) == -1);

    ControllerAssignments replacement;
    std::array<bool, 5> sources = {false, false, false, false, true};
    assert((replacement.Update(sources) == std::array<bool, 4> {}));
    assert(replacement.ControllerForSource(4) == 0);
    sources = {false, true, false, false, false};
    assert((replacement.Update(sources) == std::array<bool, 4> {true, false, false, false}));
    assert(replacement.ControllerForSource(4) == -1);
    assert(replacement.ControllerForSource(1) == 0);
    assert((replacement.Update(sources) == std::array<bool, 4> {}));

    sources = {false, true, true, true, true};
    replacement.Update(sources);
    sources = {true, false, true, true, true};
    assert((replacement.Update(sources) == std::array<bool, 4> {true, false, false, false}));
    assert(replacement.ControllerForSource(0) == 0);
    assert(replacement.ControllerForSource(1) == -1);
    assert(replacement.ControllerForSource(2) == 1);
    assert(replacement.ControllerForSource(3) == 2);
    assert(replacement.ControllerForSource(4) == 3);

    sources.fill(true);
    replacement.Update(sources);
    assert(replacement.ControllerForSource(1) == -1);
    sources[3] = false;
    assert((replacement.Update(sources) == std::array<bool, 4> {false, false, true, false}));
    assert(replacement.ControllerForSource(1) == 2);
    assert(replacement.ControllerForSource(4) == 3);

    assert((replacement.Update({}) == std::array<bool, 4> {true, true, true, true}));
    for (std::size_t iteration = 0; iteration < 100; ++iteration)
    {
        const auto source = iteration % sources.size();
        sources.fill(false);
        sources[source] = true;
        replacement.Update(sources);
        for (std::size_t index = 0; index < sources.size(); ++index)
            assert(replacement.ControllerForSource(index) == (index == source ? 0 : -1));
        assert((replacement.Update({}) == std::array<bool, 4> {true, false, false, false}));
    }

    const std::array<bool, 4> connected = {true, false, true, true};
    assert(ControllerBitmap(connected) == static_cast<std::uint16_t>(0x0d0d));
    return 0;
}
