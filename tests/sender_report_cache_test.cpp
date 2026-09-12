#include "webrtc/sender_report_cache.hpp"

#include <cassert>

int main()
{
    using Cache = opennow::webrtc::SenderReportCache;
    Cache cache;
    assert(cache.size() == 0);
    assert(!cache.find(1));

    for (uint32_t ssrc = 1; ssrc <= 100; ++ssrc) {
        cache.remember({ssrc, ssrc * 1000, ssrc * 90}, 0, 0);
        assert(cache.size() <= Cache::Capacity);
        assert(cache.find(ssrc));
    }
    assert(cache.size() == Cache::Capacity);
    assert(!cache.find(94));
    assert(cache.find(95));

    cache.remember({95, 2000, 180}, 95, 96);
    assert(cache.size() == Cache::Capacity);
    assert(cache.find(95)->ntp_us == 2000);
    assert(cache.find(95)->rtp_timestamp == 180);
    for (uint32_t ssrc = 101; ssrc < 10000; ++ssrc) {
        cache.remember({ssrc, ssrc * 1000, ssrc * 90}, 95, 96);
        assert(cache.size() == Cache::Capacity);
        assert(cache.find(95)->ntp_us == 2000);
        assert(cache.find(96)->ntp_us == 96000);
        assert(cache.find(ssrc));
    }
    assert(!cache.find(9995));
    assert(cache.find(9996));

    for (uint32_t ssrc = 10000; ssrc < 10010; ++ssrc)
        cache.remember({ssrc, ssrc * 1000, ssrc * 90}, 9999, 96);
    assert(!cache.find(95));
    assert(cache.find(9999));
    assert(cache.find(96));
    cache.remember({20000, 123, 456}, 20000, 96);
    for (uint32_t ssrc = 20001; ssrc < 20100; ++ssrc)
        cache.remember({ssrc, ssrc, ssrc}, 20000, 96);
    assert(cache.find(20000)->ntp_us == 123);
    assert(cache.find(20000)->rtp_timestamp == 456);
    assert(cache.find(96));
    assert(!cache.find(9999));

    Cache shared_ssrc;
    shared_ssrc.remember({7, 1, 1}, 7, 7);
    for (uint32_t ssrc = 10; ssrc < 100; ++ssrc)
        shared_ssrc.remember({ssrc, ssrc, ssrc}, 7, 7);
    assert(shared_ssrc.find(7));
    assert(shared_ssrc.size() == Cache::Capacity);
}
