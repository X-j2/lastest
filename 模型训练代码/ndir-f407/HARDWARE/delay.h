#ifndef __DELAY_H
#define __DELAY_H

#include "stm32f4xx.h"

/* 使用 SysTick 产生 1 ms 系统节拍。 */
void Delay_Init(void);

/* 毫秒级阻塞延时，仅用于当前无操作系统的主循环。 */
void Delay_ms(uint32_t milliseconds);

#endif
