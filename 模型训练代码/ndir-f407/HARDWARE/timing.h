#ifndef __TIMING_H
#define __TIMING_H

#include "stm32f4xx.h"

/*
 * DWT 周期计数器 + 采样时基统计 —— 调试工具
 * ============================================================
 * Cortex-M4 内置 DWT 的 CYCCNT 寄存器随内核时钟自增，精度到单个 CPU 周期，
 * 在 168 MHz 下 1 ms = 168,000 周期。用它可以在不接示波器的情况下实测：
 *
 *   1. 采样节拍间隔  —— 验证 TIM3 是否精确输出 40 ms（应为 6,720,000 周期）
 *   2. 采样回调耗时  —— ISR 内 ADC + 滤波 + NDIR 换算的执行时间
 *   3. OLED 刷新耗时 —— 软件位翻转 I2C 发送 1024 字节的真实开销
 *
 * DWT 计数器 32 位，约 25.6 秒回绕一次，远大于 40 ms 的采样间隔，无影响。
 *
 * 编译开关：只有定义 TIMING_DEBUG 时才有实际内容（在 1.uvprojx 的
 * Define 中加入 TIMING_DEBUG 即可启用），否则全部退化为空操作，
 * 不占用任何代码空间也不产生运行时开销。
 *
 * 启用后每满 1 秒经串口输出一行统计文本（此时不再发送二进制波形帧）。
 * ============================================================
 */

#ifdef TIMING_DEBUG

typedef struct
{
    uint32_t tick_count;         /* 已记录节拍数 */
    uint32_t interval_min_cyc;   /* 相邻节拍最小间隔（CPU 周期） */
    uint32_t interval_max_cyc;   /* 相邻节拍最大间隔 */
    uint32_t interval_last_cyc;  /* 最近一次间隔 */
    uint32_t interval_sum_cyc;   /* 间隔累加，用于算均值 */
    uint32_t isr_max_cyc;        /* 采样回调自身最大耗时 */
    uint32_t oled_max_cyc;       /* OLED 刷新最大耗时 */
    uint32_t queue_overflow;     /* 采样队列累计丢弃点数 */
} Timing_Stats;

void Timing_Init(void);
uint32_t Timing_Now(void);

/* 采样回调入口调用：记录本次节拍，统计与上一拍的间隔。 */
void Timing_MarkTick(void);

/* 采样回调出口调用：统计回调自身耗时。 */
void Timing_RecordIsr(uint32_t start_cycles);

/* OLED 刷新前后调用：统计刷屏耗时。 */
void Timing_RecordOled(uint32_t start_cycles);

/* 记录采样队列溢出计数（主循环每次发送帧前更新）。 */
void Timing_SetOverflow(uint32_t overflow);

void Timing_GetStats(Timing_Stats *out);
void Timing_Reset(void);

#else   /* 未启用：全部退化为空操作 */

#define Timing_Init()                 ((void)0)
#define Timing_Now()                  (0U)
#define Timing_MarkTick()             ((void)0)
#define Timing_RecordIsr(s)           ((void)0)
#define Timing_RecordOled(s)          ((void)0)
#define Timing_SetOverflow(o)         ((void)0)

#endif  /* TIMING_DEBUG */

#endif
