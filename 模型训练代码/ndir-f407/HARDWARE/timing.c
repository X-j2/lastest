#include <stddef.h>
#include "timing.h"

#ifdef TIMING_DEBUG

static volatile uint32_t s_last_tick_cyc = 0U;
static volatile uint8_t  s_has_last = 0U;

static volatile Timing_Stats s_stats;

void Timing_Init(void)
{
    uint8_t index;
    uint8_t *raw = (uint8_t *)&s_stats;

    /* 使能跟踪与调试模块，再启动周期计数器。 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    for (index = 0U; index < (uint8_t)sizeof(s_stats); index++)
    {
        raw[index] = 0U;
    }

    /* 最小间隔初值取最大可能值，便于第一拍即可正确更新。 */
    s_stats.interval_min_cyc = 0xFFFFFFFFU;
    s_has_last = 0U;
}

uint32_t Timing_Now(void)
{
    return DWT->CYCCNT;
}

void Timing_MarkTick(void)
{
    uint32_t now = DWT->CYCCNT;

    if (s_has_last == 0U)
    {
        s_has_last = 1U;
        s_last_tick_cyc = now;
        return;
    }

    /* 无符号相减，DWT 32 位回绕（约 25.6 秒）自动正确 */
    {
        uint32_t interval = now - s_last_tick_cyc;

        s_last_tick_cyc = now;
        s_stats.interval_last_cyc = interval;
        s_stats.interval_sum_cyc += interval;
        s_stats.tick_count++;

        if (interval < s_stats.interval_min_cyc)
        {
            s_stats.interval_min_cyc = interval;
        }
        if (interval > s_stats.interval_max_cyc)
        {
            s_stats.interval_max_cyc = interval;
        }
    }
}

void Timing_RecordIsr(uint32_t start_cycles)
{
    uint32_t elapsed = DWT->CYCCNT - start_cycles;

    if (elapsed > s_stats.isr_max_cyc)
    {
        s_stats.isr_max_cyc = elapsed;
    }
}

void Timing_RecordOled(uint32_t start_cycles)
{
    uint32_t elapsed = DWT->CYCCNT - start_cycles;

    if (elapsed > s_stats.oled_max_cyc)
    {
        s_stats.oled_max_cyc = elapsed;
    }
}

void Timing_SetOverflow(uint32_t overflow)
{
    s_stats.queue_overflow = overflow;
}

void Timing_GetStats(Timing_Stats *out)
{
    if (out == NULL)
    {
        return;
    }

    *out = *(const Timing_Stats *)&s_stats;
}

void Timing_Reset(void)
{
    uint8_t index;
    uint8_t *raw = (uint8_t *)&s_stats;

    for (index = 0U; index < (uint8_t)sizeof(s_stats); index++)
    {
        raw[index] = 0U;
    }

    s_stats.interval_min_cyc = 0xFFFFFFFFU;
    s_has_last = 0U;
}

#endif /* TIMING_DEBUG */
