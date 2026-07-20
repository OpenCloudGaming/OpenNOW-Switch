#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

struct mbedtls_timing_hr_time
{
    unsigned char opaque[32];
};

typedef struct mbedtls_timing_delay_context
{
    struct mbedtls_timing_hr_time timer;
    uint32_t int_ms;
    uint32_t fin_ms;
} mbedtls_timing_delay_context;

static uint64_t opennow_now_ms(void)
{
#ifdef __SWITCH__
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
#else
    return (uint64_t)((clock() * 1000ULL) / CLOCKS_PER_SEC);
#endif
}

unsigned long mbedtls_timing_get_timer(struct mbedtls_timing_hr_time* val, int reset)
{
    uint64_t start_ms = 0;
    const uint64_t now_ms = opennow_now_ms();

    memcpy(&start_ms, val->opaque, sizeof(start_ms));
    if (reset || start_ms == 0)
    {
        memcpy(val->opaque, &now_ms, sizeof(now_ms));
        return 0;
    }

    return (unsigned long)(now_ms - start_ms);
}

void mbedtls_timing_set_delay(void* data, uint32_t int_ms, uint32_t fin_ms)
{
    mbedtls_timing_delay_context* ctx = (mbedtls_timing_delay_context*)data;
    ctx->int_ms = int_ms;
    ctx->fin_ms = fin_ms;

    if (fin_ms != 0)
        (void)mbedtls_timing_get_timer(&ctx->timer, 1);
}

int mbedtls_timing_get_delay(void* data)
{
    mbedtls_timing_delay_context* ctx = (mbedtls_timing_delay_context*)data;
    if (ctx->fin_ms == 0)
        return -1;

    const unsigned long elapsed_ms = mbedtls_timing_get_timer(&ctx->timer, 0);
    if (elapsed_ms >= ctx->fin_ms)
        return 2;

    if (elapsed_ms >= ctx->int_ms)
        return 1;

    return 0;
}

uint32_t mbedtls_timing_get_final_delay(const mbedtls_timing_delay_context* data)
{
    return data->fin_ms;
}
