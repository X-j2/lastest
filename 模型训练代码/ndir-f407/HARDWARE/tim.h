#ifndef __TIM_PWM_H
#define __TIM_PWM_H

#include "stm32f4xx.h"

/* 初始化 PA0 上的 TIM2_CH1 PWM 输出。 */
void TIM2_PWM_Init(void);

/* 修改 PWM 占空比，有效范围为 0% 到 100%。 */
void TIM2_PWM_SetDuty(uint8_t duty_percent);

/*
 * 初始化 TIM3 采样节拍：以精确 25 Hz 触发更新中断。
 *
 * 为什么用中断而不是主循环延时：主循环里 OLED 软件 I2C 刷新、串口整帧发送、
 * AI 满窗推理都是耗时且时长不定的操作。若靠 Delay_ms 维持节拍，真实采样周期
 * 会变成"循环耗时 + 延时"，采样率既偏低又随内容抖动。改用硬件定时器中断后，
 * 采样节拍与主循环耗时彻底解耦，FE_RAW_RATE=25 / 90 秒 = 2250 点 的假设才成立。
 */
void TIM3_SampleTimer_Init(void);

/*
 * 采样节拍回调，由 TIM3 中断以精确 25 Hz 调用，需在应用侧实现。
 *
 * 约束：实现内只做"采样 + 入队"，不得调用 OLED、串口等耗时操作，
 * 也不得调用任何可能阻塞的接口。中断里耗时越长，越容易挤压主循环。
 */
void App_OnSampleTick(void);

#endif
