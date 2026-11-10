#include <stddef.h>
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_usart.h"
#include "usart.h"

/* 发送一个字节，并等待发送数据寄存器空。 */
static void USART1_SendByte(uint8_t data)
{
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET)
    {
    }

    USART_SendData(USART1, data);
}

void USART1_Waveform_Init(void)
{
    GPIO_InitTypeDef gpio_init;
    USART_InitTypeDef usart_init;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);

    /* F4 用 AFRL 选择 PA9 的复用功能 USART1_TX。 */
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);

    /* PA9/USART1_TX 使用复用推挽输出，连接 USB 转串口模块的 RX。 */
    gpio_init.GPIO_Pin = GPIO_Pin_9;
    gpio_init.GPIO_Mode = GPIO_Mode_AF;
    gpio_init.GPIO_OType = GPIO_OType_PP;
    gpio_init.GPIO_PuPd = GPIO_PuPd_UP;
    gpio_init.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_Init(GPIOA, &gpio_init);

    USART_StructInit(&usart_init);
    usart_init.USART_BaudRate = USART1_BAUD_RATE;
    usart_init.USART_WordLength = USART_WordLength_8b;
    usart_init.USART_StopBits = USART_StopBits_1;
    usart_init.USART_Parity = USART_Parity_No;
    usart_init.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart_init.USART_Mode = USART_Mode_Tx;
    USART_Init(USART1, &usart_init);
    USART_Cmd(USART1, ENABLE);
}

/*
 * 发送64字节波形帧：
 * 字节0为0xFF帧头；字节1～10为5个CO₂浓度采样点（ppm，大端序）；
 * 字节11为AI标志（0xAA=已有结果，0x00=暂无）；字节12为分类结果（0..3）；
 * 字节13～14为呼吸率×10（大端序）；字节15为字节0～14累加和的低8位；
 * 字节16～19为采样队列累计丢弃点数（大端序，正常恒为0）；
 * 字节20～63为保留字节0。
 *
 * ---- 协议 v2.3 变更（2026-09）----
 * 字节1～10 由「ACT通道电压(mV)」改为「CO₂浓度(ppm)」。
 *
 * 为什么改：原先上位机只拿到 ACT 一路绝对电压，用它做了一套独立标定
 * （×增益×7.5006 → mmHg）；而固件用的是 act/ref 比值经两点标定得 ppm。
 * 比值法能消除光源老化的影响，绝对电压法不能——两套标定互不相干，
 * 上位机画出的波形本身就比固件差。改为直接下发浓度后，两端同源。
 *
 * ⚠ 帧长度、字节偏移、校验和均未变，因此校验和照样通过。
 *   旧上位机会把 ppm 当成电压（ppm×0.0015 ≈ 伪 mmHg），
 *   数值完全错误却不报错。**固件与上位机必须同步升级。**
 *
 * 溢出计数存在的意义：采样由定时器中断产生、主循环异步消费，
 * 若主循环阻塞过久（例如 AI 满窗推理超预期）导致队列满，点会被丢弃。
 * 把计数透出来，上位机可据此判断该窗口的特征是否可信——避免出现
 * "不报错但结果错"的静默失效。
 */
void USART1_SendWaveformFrame(const uint16_t samples_ppm[5U],
                              const AI_Status *ai_status,
                              uint32_t overflow_count)
{
    uint8_t frame[64U];
    uint8_t index;

    /* 先清零整帧，确保字节16～63全部为0x00。 */
    for (index = 0U; index < 64U; index++)
    {
        frame[index] = 0U;
    }

    frame[0U] = 0xFFU;

    /* 5 个浓度采样点，各为 uint16 大端序（ppm） */
    for (index = 0U; index < 5U; index++)
    {
        frame[index * 2U + 1U] = (uint8_t)(samples_ppm[index] >> 8U);
        frame[index * 2U + 2U] = (uint8_t)(samples_ppm[index] & 0xFFU);
    }

    /* 附带AI诊断结果；首次推理完成前保持 0x00。 */
    if ((ai_status != NULL) && (ai_status->valid != 0U))
    {
        frame[11U] = 0xAAU;
        frame[12U] = ai_status->label;
        frame[13U] = (uint8_t)(ai_status->resp_rate_x10 >> 8U);
        frame[14U] = (uint8_t)(ai_status->resp_rate_x10 & 0xFFU);
    }
    else
    {
        frame[11U] = 0x00U;
        frame[12U] = 0x00U;
        frame[13U] = 0x00U;
        frame[14U] = 0x00U;
    }

    /* 采样队列累计丢弃点数，大端序。正常恒为 0。 */
    frame[16U] = (uint8_t)((overflow_count >> 24U) & 0xFFU);
    frame[17U] = (uint8_t)((overflow_count >> 16U) & 0xFFU);
    frame[18U] = (uint8_t)((overflow_count >> 8U) & 0xFFU);
    frame[19U] = (uint8_t)(overflow_count & 0xFFU);

    /* 校验范围包括帧头和前15个字节，不包括校验字节本身。 */
    frame[15U] = 0U;
    for (index = 0U; index < 15U; index++)
    {
        frame[15U] = (uint8_t)(frame[15U] + frame[index]);
    }

    for (index = 0U; index < 64U; index++)
    {
        USART1_SendByte(frame[index]);
    }
}

#ifdef TIMING_DEBUG
/* ------------------------------------------------------------------
 * 时基统计文本输出
 *
 * 全部用整数运算拼接，不调用 printf/sprintf：MicroLIB 的浮点格式化会
 * 额外拉入数 KB 代码，而这些数字用整数毫秒/微秒表达已足够精确。
 *
 * 输出形如（每行约 1 秒一条）：
 *   tick=18125 int_min=40ms int_max=40ms isr_max=1ms oled_max=88ms ovf=0
 *
 * 读数含义：
 *   tick      累计采样节拍数，约 1 秒增长 25
 *   int_min   相邻采样最小间隔，精确 25Hz 时应为 40
 *   int_max   相邻采样最大间隔，与 int_min 相等即说明零抖动
 *   isr_max   采样回调自身最大耗时
 *   oled_max  OLED 刷屏最大耗时
 *   ovf       采样队列累计丢弃点数，正常恒为 0
 * ------------------------------------------------------------------ */
static void USART1_SendUInt(uint32_t value)
{
    char    digits[11];
    uint8_t count = 0U;

    if (value == 0U)
    {
        USART1_SendByte('0');
        return;
    }

    while ((value > 0U) && (count < 10U))
    {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        count++;
    }

    while (count > 0U)
    {
        count--;
        USART1_SendByte((uint8_t)digits[count]);
    }
}

static void USART1_SendLabel(const char *label)
{
    while (*label != '\0')
    {
        USART1_SendByte((uint8_t)(*label));
        label++;
    }
}

/*
 * 把 CPU 周期换算成毫秒输出。
 * 168 MHz 下 1 ms = 168,000 周期，25 Hz 的采样节拍 = 40 ms = 6,720,000 周期。
 * 因此 int_min/int_max 正常读数应为 40；偏大说明节拍变慢，偏小说明变快。
 */
static void USART1_SendMilliseconds(uint32_t cycles)
{
    USART1_SendUInt(cycles / (SystemCoreClock / 1000U));
}

void USART1_SendTimingReport(const Timing_Stats *stats)
{
    if (stats == NULL)
    {
        return;
    }

    USART1_SendLabel("tick=");
    USART1_SendUInt(stats->tick_count);

    USART1_SendLabel(" int_min=");
    USART1_SendMilliseconds(stats->interval_min_cyc);

    USART1_SendLabel("ms int_max=");
    USART1_SendMilliseconds(stats->interval_max_cyc);

    USART1_SendLabel("ms isr_max=");
    USART1_SendMilliseconds(stats->isr_max_cyc);

    USART1_SendLabel("ms oled_max=");
    USART1_SendMilliseconds(stats->oled_max_cyc);

    USART1_SendLabel("ms ovf=");
    USART1_SendUInt(stats->queue_overflow);

    USART1_SendLabel("\r\n");
}
#endif /* TIMING_DEBUG */
