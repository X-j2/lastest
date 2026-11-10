#include "delay.h"

static volatile uint32_t delay_ticks = 0U;

void Delay_Init(void)
{
    SystemCoreClockUpdate();
    (void)SysTick_Config(SystemCoreClock / 1000U);
}

void Delay_ms(uint32_t milliseconds)
{
    delay_ticks = milliseconds;
    while (delay_ticks != 0U)
    {
        /* 等待 SysTick 中断递减计数值。 */
    }
}

void SysTick_Handler(void)
{
    if (delay_ticks != 0U)
    {
        delay_ticks--;
    }
}
