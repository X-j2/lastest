#include "stm32f4xx.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_tim.h"
#include "misc.h"
#include "tim.h"

#define PWM_TIMER              TIM2              /* PWM 使用的定时器。 */
#define PWM_TIMER_RCC          RCC_APB1Periph_TIM2
#define PWM_GPIO               GPIOA             /* TIM2_CH1 输出端口。 */
#define PWM_GPIO_RCC           RCC_AHB1Periph_GPIOA
#define PWM_GPIO_PIN           GPIO_Pin_0        /* TIM2_CH1 输出引脚 PA0。 */
#define PWM_GPIO_PIN_SOURCE    GPIO_PinSource0
#define PWM_GPIO_AF            GPIO_AF_TIM2

/*
 * 系统时钟 168 MHz，APB1 分频为 4 时 APB1 = 42 MHz，F4 的定时器时钟
 * 在 APB 分频不为 1 时自动乘以 2，因此 TIM2 实际计数时钟为 84 MHz。
 *
 * PWM 频率 = 定时器时钟 / (预分频值 + 1) / (自动重装值 + 1)
 *          = 84,000,000 / 8,400 / 10,000 = 1 Hz
 */
#define PWM_PRESCALER          8399U             /* 84 MHz / 8,400 = 10 kHz。 */
#define PWM_PERIOD             9999U             /* 10 kHz / 10,000 = 1 Hz。 */

void TIM2_PWM_Init(void)
{
    GPIO_InitTypeDef gpio_init;
    TIM_TimeBaseInitTypeDef timebase_init;
    TIM_OCInitTypeDef oc_init;

    /* 开启 GPIOA 和 TIM2 的外设时钟。 */
    RCC_AHB1PeriphClockCmd(PWM_GPIO_RCC, ENABLE);
    RCC_APB1PeriphClockCmd(PWM_TIMER_RCC, ENABLE);

    /* F4 用 AFRL 选择 PA0 的复用功能 TIM2_CH1。 */
    GPIO_PinAFConfig(PWM_GPIO, PWM_GPIO_PIN_SOURCE, PWM_GPIO_AF);

    /* 将 PA0 配置为复用推挽输出。 */
    gpio_init.GPIO_Pin = PWM_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_AF;
    gpio_init.GPIO_OType = GPIO_OType_PP;
    gpio_init.GPIO_PuPd = GPIO_PuPd_UP;
    gpio_init.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_Init(PWM_GPIO, &gpio_init);

    /* 配置定时器计数参数，用于确定 PWM 输出频率。 */
    TIM_TimeBaseStructInit(&timebase_init);
    timebase_init.TIM_Prescaler = PWM_PRESCALER;
    timebase_init.TIM_CounterMode = TIM_CounterMode_Up;
    timebase_init.TIM_Period = PWM_PERIOD;
    timebase_init.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInit(PWM_TIMER, &timebase_init);

    /* 配置通道 1 为 PWM 模式 1，初始占空比为 50%。 */
    TIM_OCStructInit(&oc_init);
    oc_init.TIM_OCMode = TIM_OCMode_PWM1;
    oc_init.TIM_OutputState = TIM_OutputState_Enable;
    oc_init.TIM_Pulse = (PWM_PERIOD + 1U) * 50U / 100U;
    oc_init.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OC1Init(PWM_TIMER, &oc_init);
    /* 开启预装载，使比较值和周期值在下一个周期统一更新。 */
    TIM_OC1PreloadConfig(PWM_TIMER, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(PWM_TIMER, ENABLE);

    /* 在启动定时器前立即装载 PSC、ARR 和 CCR1，避免首次输出延迟。 */
    TIM_SetCounter(PWM_TIMER, 0U);
    TIM_GenerateEvent(PWM_TIMER, TIM_EventSource_Update);
    TIM_ClearFlag(PWM_TIMER, TIM_FLAG_Update);

    /* 启动 TIM2，PWM 波形开始从 PA0 输出。 */
    TIM_Cmd(PWM_TIMER, ENABLE);
}

/* 设置 PWM 占空比，输入范围为 0% 到 100%。 */
void TIM2_PWM_SetDuty(uint8_t duty_percent)
{
    if (duty_percent > 100U)
    {
        duty_percent = 100U;
    }

    /* CCR1 = (周期值 + 1) * 占空比 / 100。 */
    TIM_SetCompare1(PWM_TIMER,
                    (uint16_t)((PWM_PERIOD + 1U) * duty_percent / 100U));
}

/* ==================================================================
 * TIM3 采样节拍：25 Hz 更新中断
 * ================================================================== */

#define SAMPLE_TIMER              TIM3
#define SAMPLE_TIMER_RCC          RCC_APB1Periph_TIM3
#define SAMPLE_TIMER_IRQn         TIM3_IRQn

/*
 * 与 TIM2 同源：APB1 = 42 MHz，定时器时钟自动 ×2 = 84 MHz。
 *
 * 采样频率 = 84,000,000 / (8399 + 1) / (399 + 1)
 *          = 84,000,000 / 8,400 / 400
 *          = 25 Hz        （周期 40.000 ms）
 */
#define SAMPLE_PRESCALER          8399U   /* 84 MHz / 8,400 = 10 kHz */
#define SAMPLE_PERIOD              399U   /* 10 kHz / 400 = 25 Hz */

void TIM3_SampleTimer_Init(void)
{
    TIM_TimeBaseInitTypeDef timebase_init;
    NVIC_InitTypeDef nvic_init;

    RCC_APB1PeriphClockCmd(SAMPLE_TIMER_RCC, ENABLE);

    TIM_TimeBaseStructInit(&timebase_init);
    timebase_init.TIM_Prescaler = SAMPLE_PRESCALER;
    timebase_init.TIM_CounterMode = TIM_CounterMode_Up;
    timebase_init.TIM_Period = SAMPLE_PERIOD;
    timebase_init.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInit(SAMPLE_TIMER, &timebase_init);

    /* 先清掉配置过程中可能置起的更新标志，避免一开中断立刻误触发一次。 */
    TIM_ClearFlag(SAMPLE_TIMER, TIM_FLAG_Update);
    TIM_ITConfig(SAMPLE_TIMER, TIM_IT_Update, ENABLE);

    nvic_init.NVIC_IRQChannel = SAMPLE_TIMER_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 0U;
    nvic_init.NVIC_IRQChannelSubPriority = 0U;
    nvic_init.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic_init);

    TIM_SetCounter(SAMPLE_TIMER, 0U);
    TIM_Cmd(SAMPLE_TIMER, ENABLE);
}

/*
 * TIM3 更新中断：每 40.000 ms 触发一次采样。
 * 与 SysTick_Handler 一样放在模块文件里，避免 stm32f4xx_it.c 重复定义。
 */
void TIM3_IRQHandler(void)
{
    if (TIM_GetITStatus(SAMPLE_TIMER, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(SAMPLE_TIMER, TIM_IT_Update);
        App_OnSampleTick();
    }
}
