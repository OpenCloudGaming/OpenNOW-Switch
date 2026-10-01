#pragma once

#include <cstddef>
#include <cstdint>

namespace opennow::audio {

struct ParsedPayload {
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool red = false;
};

struct RedundantPayload {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint16_t timestamp_offset = 0;
};

inline RedundantPayload ParseLatestRedundant(const uint8_t* data, size_t size) {
    if (!data || size < 5 || (data[0] & 0x80) == 0)
        return {};
    size_t header = 0;
    size_t redundant_bytes = 0;
    size_t length = 0;
    uint16_t timestamp_offset = 0;
    while (header < size && (data[header] & 0x80) != 0) {
        if (header + 4 > size || (data[header] & 0x7f) != 111)
            return {};
        length = size_t(((data[header + 2] & 0x03) << 8) | data[header + 3]);
        redundant_bytes += length;
        if (length == 0 || redundant_bytes > size)
            return {};
        timestamp_offset =
            uint16_t((uint16_t(data[header + 1]) << 6) | (data[header + 2] >> 2));
        header += 4;
    }
    if (header >= size || (data[header] & 0x7f) != 111)
        return {};
    ++header;
    if (redundant_bytes >= size - header)
        return {};
    return {data + header + redundant_bytes - length, length, timestamp_offset};
}

inline ParsedPayload ParseRedPrimary(const uint8_t* data, size_t size, uint8_t payload_type) {
    if (!data || size == 0)
        return {};
    if (payload_type != 63)
        return {data, size, false};

    size_t header = 0;
    size_t redundant_bytes = 0;
    while (header < size && (data[header] & 0x80) != 0) {
        if (header + 4 > size)
            return {};
        redundant_bytes += size_t(((data[header + 2] & 0x03) << 8) | data[header + 3]);
        header += 4;
    }
    if (header >= size)
        return {};
    ++header;
    if (header + redundant_bytes >= size)
        return {};
    return {data + header + redundant_bytes, size - header - redundant_bytes, true};
}

constexpr int64_t RtpDeltaToUs(uint32_t value, uint32_t anchor, uint32_t clock_rate) {
    return static_cast<int64_t>(static_cast<int32_t>(value - anchor)) * 1000000LL /
           static_cast<int64_t>(clock_rate);
}

constexpr uint32_t AudioTimelineBase(uint32_t first_packet_timestamp, uint64_t pre_roll_samples) {
    return first_packet_timestamp - static_cast<uint32_t>(pre_roll_samples);
}

constexpr uint32_t RtpTimestampAtNtp(uint32_t anchor_rtp, int64_t anchor_ntp_us,
                                     int64_t target_ntp_us, uint32_t clock_rate) {
    const int64_t delta_us = target_ntp_us - anchor_ntp_us;
    return anchor_rtp + static_cast<uint32_t>(delta_us * static_cast<int64_t>(clock_rate) / 1000000LL);
}

constexpr bool ShouldResetEpoch(uint64_t previous_arrival_us, uint64_t arrival_us,
                                bool have_expected, uint16_t expected_sequence,
                                uint16_t sequence, int max_packets) {
    if (previous_arrival_us != 0 && arrival_us > previous_arrival_us &&
        arrival_us - previous_arrival_us > 500000)
        return true;
    return have_expected && static_cast<int16_t>(sequence - expected_sequence) > max_packets * 4;
}

} // namespace opennow::audio
