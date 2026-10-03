#include "library_timetable_policy.hpp"

#include <cassert>
#include <chrono>

int main()
{
    using namespace std::chrono;
    using opennow::library::DateBucket;
    using opennow::library::PresentTimestamp;
    const auto now = sys_days{year{2026} / October / 2} + hours{12};
    assert(PresentTimestamp("2026-10-02T00:00:00Z", now).bucket == DateBucket::Today);
    assert(PresentTimestamp("2026-10-02T11:59:59.999Z", now).label == "11:59");
    assert(PresentTimestamp("2026-10-01T23:59:59Z", now).bucket == DateBucket::ThisWeek);
    assert(PresentTimestamp("2026-09-28T00:00:00Z", now).label == "Mon");
    assert(PresentTimestamp("2026-09-27T23:59:59Z", now).bucket == DateBucket::Earlier);
    assert(PresentTimestamp("2025-10-02T09:15:00Z", now).label == "2025-10-02");
    assert(PresentTimestamp("", now).bucket == DateBucket::Unplayed);
    for (const auto* text : {"2026-10-02T12:00:01Z", "2026-10-02T12:00:00.001Z",
            "2026-02-29T09:00:00Z", "2026-13-01T00:00:00Z", "2026-04-31T00:00:00Z",
            "2026-10-02T24:00:00Z", "2026-10-02T10:60:00Z", "2026-10-02T10:00:60Z",
            "2026-10-02T10:00:00", "2026-10-02T10:00:00+00:00", "2026-10-02T10:00:00.Z",
            "2026-10-02T10:00:00.1234567890Z", "2026-10-02T10:00:00.aZ",
            "2026-10-02T10:00:00Zextra", "unknown", "0000-01-01T00:00:00Z"})
        assert(PresentTimestamp(text, now).bucket == DateBucket::Unknown);
    assert(PresentTimestamp("2024-02-29T09:00:00Z", now).bucket == DateBucket::Earlier);
    assert(PresentTimestamp("2026-10-02T12:00:00.000000000Z", now).bucket == DateBucket::Today);
    const auto midnight = sys_days{year{2026} / October / 5};
    assert(PresentTimestamp("2026-10-04T23:59:59Z", midnight).bucket == DateBucket::Earlier);
    assert(PresentTimestamp("2026-10-05T00:00:00Z", midnight).bucket == DateBucket::Today);
    assert(PresentTimestamp("2026-10-05T00:00:00.1Z", midnight).bucket == DateBucket::Unknown);
}
