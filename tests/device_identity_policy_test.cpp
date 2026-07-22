#include "device_identity_policy.hpp"

#include <cassert>

static_assert(!opennow::device_identity::IsUsableStoredDeviceId(""));
static_assert(!opennow::device_identity::IsUsableStoredDeviceId(
    "12345678-1234-5678-1234-567812345678"));
static_assert(opennow::device_identity::IsUsableStoredDeviceId(
    "2b14829e-1bd6-49b9-8e92-cf25d03492c4"));

int main()
{
    assert(opennow::device_identity::IsUsableStoredDeviceId("legacy-unique-id"));
    return 0;
}
