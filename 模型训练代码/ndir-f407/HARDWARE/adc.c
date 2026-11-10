#include "stm32f4xx_adc.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_rcc.h"
#include "adc.h"

void ADC_DualChannel_Init(void)
{
    GPIO_InitTypeDef gpio_init;
    ADC_CommonInitTypeDef adc_common_init;
    ADC_InitTypeDef adc_init;
    volatile uint32_t wait_loop;

    /* APB2 = 168 MHz / 2 = 84 MHz；ADC 时钟取 4 分频 = 21 MHz（上限 36 MHz）。 */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);

    /* PA1 和 PA2 配置成模拟输入。 */
    gpio_init.GPIO_Pin = GPIO_Pin_1 | GPIO_Pin_2;
    gpio_init.GPIO_Mode = GPIO_Mode_AN;
    gpio_init.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOA, &gpio_init);

    /* F4 的 ADC 公共配置必须先于 ADC_Init 设置，ADC 时钟分频在此确定。 */
    ADC_CommonStructInit(&adc_common_init);
    adc_common_init.ADC_Mode = ADC_Mode_Independent;
    adc_common_init.ADC_Prescaler = ADC_Prescaler_Div4;
    adc_common_init.ADC_DMAAccessMode = ADC_DMAAccessMode_Disabled;
    adc_common_init.ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles;
    ADC_CommonInit(&adc_common_init);

    ADC_StructInit(&adc_init);
    adc_init.ADC_Resolution = ADC_Resolution_12b;
    adc_init.ADC_ScanConvMode = DISABLE;
    adc_init.ADC_ContinuousConvMode = DISABLE;
    adc_init.ADC_ExternalTrigConvEdge = ADC_ExternalTrigConvEdge_None;
    adc_init.ADC_ExternalTrigConv = ADC_ExternalTrigConv_T1_CC1;
    adc_init.ADC_DataAlign = ADC_DataAlign_Right;
    adc_init.ADC_NbrOfConversion = 1U;
    ADC_Init(ADC1, &adc_init);

    /* 使能 ADC（置位 ADON 同时接通内部稳压器）。 */
    ADC_Cmd(ADC1, ENABLE);

    /*
     * 等待内部稳压器稳定。F4 参考手册要求上电后延时约 10 us 再启动首次转换。
     * 本工程所用标准库为精简版，未提供 ADC_VoltageRegulatorCmd /
     * ADC_StartCalibration 等软件校准 API，因此此处只做稳压器稳定延时，
     * 不做软件校准——不影响本样机的趋势测量（12 位精度下方差极小）。
     */
    for (wait_loop = 0U; wait_loop < 3000U; wait_loop++)
    {
        __NOP();
    }
}

static uint16_t ADC_ReadOnce(uint8_t adc_channel)
{
    /* 信号变化很慢，使用较长采样时间提高对高阻信号源的兼容性。 */
    ADC_RegularChannelConfig(ADC1,
                             adc_channel,
                             1U,
                             ADC_SampleTime_480Cycles);
    ADC_SoftwareStartConv(ADC1);

    while (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET)
    {
    }

    return ADC_GetConversionValue(ADC1);
}

uint16_t ADC_ReadAverage(uint8_t adc_channel, uint8_t sample_count)
{
    uint8_t index;
    uint32_t sum = 0U;

    if (sample_count == 0U)
    {
        sample_count = 1U;
    }

    for (index = 0U; index < sample_count; index++)
    {
        sum += ADC_ReadOnce(adc_channel);
    }

    return (uint16_t)((sum + (sample_count / 2U)) / sample_count);
}

uint16_t ADC_CountToMillivolt(uint16_t adc_count)
{
    return (uint16_t)(((uint32_t)adc_count * ADC_REFERENCE_MV + 2047U) / 4095U);
}
