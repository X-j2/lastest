#ifndef __USART_WAVEFORM_H
#define __USART_WAVEFORM_H

#include "stm32f4xx.h"
#include "ai_bridge.h"
#include "timing.h"

/* USART1：PA9 为发送引脚，波特率 115200，8 数据位、1 停止位、无校验。 */
#define USART1_BAUD_RATE  115200U

void USART1_Waveform_Init(void);

/* 发送 64 字节波形帧。
 *
 * ⚠ 协议 v2.3 变更（与 v2.2 及更早的上位机不兼容）：
 *    字节 1-10 由「ACT 通道电压(mV)」改为「CO₂ 浓度(ppm)」。
 *    帧长度、字节偏移、校验和均未变，因此旧上位机不会报错，
 *    只会把 ppm 当成电压算出完全错误的浓度——务必与上位机同步升级。
 *    详见 usart.c 中的帧格式说明。
 *
 * samples_ppm 为 5 个浓度采样点（ppm）；ai_status 携带 AI 诊断结果，可为 NULL
 * （为 NULL 时 AI 相关字节按“暂无结果”填充）。
 * overflow_count 为采样队列累计丢弃点数，正常情况下恒为 0。 */
void USART1_SendWaveformFrame(const uint16_t samples_ppm[5U],
                              const AI_Status *ai_status,
                              uint32_t overflow_count);

#ifdef TIMING_DEBUG
/* 输出一行时基统计文本，供串口助手直接阅读。
 * 不依赖 printf / sprintf，避免与 MicroLIB 的浮点格式化产生额外链接开销。 */
void USART1_SendTimingReport(const Timing_Stats *stats);
#endif

#endif
