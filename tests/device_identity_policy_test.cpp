#include "device_identity_policy.hpp"

#include <cassert>

static_assert(!opennow::device_identity::IsUsableStoredDeviceId(""));
static_assert(!opennow::device_identity::IsUsableStoredDeviceId(
    "12345678-1234-5678-1234-567812345678"));
static_assert(opennow::device_identity::IsUsableStoredDeviceId(
    "2b14829e-1bd6-49b9-8e92-cf25d03492c4"));
static_assert(opennow::device_identity::IsCanonicalUuid(
    "2b14829e-1bd6-49b9-8e92-cf25d03492c4"));
static_assert(opennow::device_identity::IsCanonicalUuid(
    "2B14829E-1BD6-49B9-8E92-CF25D03492C4"));
static_assert(!opennow::device_identity::IsCanonicalUuid("legacy-unique-id"));
static_assert(!opennow::device_identity::IsCanonicalUuid(
    "2b14829e1bd6-49b9-8e92-cf25d03492c4"));
static_assert(!opennow::device_identity::IsCanonicalUuid(
    "2b14829e-1bd6-49b9-8e92-cf25d03492cz"));

int main()
{
    assert(opennow::device_identity::IsUsableStoredDeviceId("legacy-unique-id"));
    assert(!opennow::device_identity::IsCanonicalUuid("legacy-unique-id"));
    return 0;
}
