#include "controller_layout.hpp"

#include <cassert>

int main()
{
    using namespace opennow;

    assert(MapFaceButtons("Xbox", true, false, false, false) == kXboxButtonB);
    assert(MapFaceButtons("Xbox", false, true, false, false) == kXboxButtonA);
    assert(MapFaceButtons("Xbox", false, false, true, false) == kXboxButtonY);
    assert(MapFaceButtons("Xbox", false, false, false, true) == kXboxButtonX);

    assert(MapFaceButtons("Switch", true, false, false, false) == kXboxButtonA);
    assert(MapFaceButtons("Switch", false, true, false, false) == kXboxButtonB);
    assert(MapFaceButtons("Switch", false, false, true, false) == kXboxButtonX);
    assert(MapFaceButtons("Switch", false, false, false, true) == kXboxButtonY);

    assert(MapFaceButtons("Switch", true, true, true, true) ==
           (kXboxButtonA | kXboxButtonB | kXboxButtonX | kXboxButtonY));
    assert(MapFaceButtons("invalid", true, false, false, false) == kXboxButtonB);
    return 0;
}
