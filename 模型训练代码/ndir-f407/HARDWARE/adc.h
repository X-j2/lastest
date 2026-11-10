#ifndef __ADC_DUAL_H
#define __ADC_DUAL_H

#include "stm32f4xx.h"

/*
 * ADC_IN_1 接 PA1/ADC1_IN1，作为 CO2 测量通道 ACT。
 * ADC_IN_2 接 PA2/ADC1_IN2，作为参考通道 REF。
 * 如果实际接线相反，只需交换下面两个通道宏。
 */
#define ADC_ACT_CHANNEL        ADC_Channel_1
#define ADC_REF_CHANNEL        ADC_Channel_2

#define ADC_REFERENCE_MV       3300U

void ADC_DualChannel_Init(void);
uint16_t ADC_ReadAverage(uint8_t adc_channel, uint8_t sample_count);
uint16_t ADC_CountToMillivolt(uint16_t adc_count);

#endif
