#include <chrono>
#include <cstdint>
#include <cstring>

#if defined(OPENNOW_HAS_LIBPEER)
#include <mbedtls/timing.h>

namespace {

struct TimingState {
    std::uint64_t startMs = 0;
    std::uint32_t intMs = 0;
    std::uint32_t finMs = 0;
};

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

TimingState readState(const void* data) {
    TimingState state;
    std::memcpy(&state, data, sizeof(state));
    return state;
}

void writeState(void* data, const TimingState& state) {
    std::memcpy(data, &state, sizeof(state));
}

} // namespace

extern "C" unsigned long mbedtls_timing_get_timer(struct mbedtls_timing_hr_time* val, int reset) {
    if (!val) {
        return 0;
    }

    std::uint64_t stored = 0;
    std::memcpy(&stored, val, sizeof(stored));
    const auto current = nowMs();
    if (reset || stored == 0) {
        std::memcpy(val, &current, sizeof(current));
        return 0;
    }
    return static_cast<unsigned long>(current - stored);
}

extern "C" void mbedtls_timing_set_delay(void* data, std::uint32_t int_ms, std::uint32_t fin_ms) {
    if (!data) {
        return;
    }

    writeState(data, TimingState{
        .startMs = nowMs(),
        .intMs = int_ms,
        .finMs = fin_ms,
    });
}

extern "C" int mbedtls_timing_get_delay(void* data) {
    if (!data) {
        return -1;
    }

    const auto state = readState(data);
    if (state.finMs == 0) {
        return -1;
    }

    const auto elapsed = nowMs() - state.startMs;
    if (elapsed >= state.finMs) {
        return 2;
    }
    if (elapsed >= state.intMs) {
        return 1;
    }
    return 0;
}

extern "C" std::uint32_t mbedtls_timing_get_final_delay(const mbedtls_timing_delay_context* data) {
    if (!data) {
        return 0;
    }
    return readState(data).finMs;
}
#endif
