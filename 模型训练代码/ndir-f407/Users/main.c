#include "stm32f4xx.h"
#include "adc.h"
#include "delay.h"
#include "ndir_math.h"
#include "oled.h"
#include "tim.h"
#include "usart.h"
#include "timing.h"
#include "ai_bridge.h"

#define ADC_AVERAGE_SAMPLES       32U

/*
 * OLED 每 10 个采样点刷新一次 = 2.5 帧/秒。
 * 呼吸波形本身很慢，2~3 帧/秒足够看趋势；软件 I2C 刷屏是全系统最重的
 * 单次操作，降低帧率可为主循环（尤其是 AI 满窗推理）留出余量。
 */
#define OLED_REFRESH_SAMPLES       10U

#define HISTORY_LENGTH            128U
#define GRAPH_TOP                  24U
#define GRAPH_BOTTOM               60U
#define GRAPH_MIN_PPM               0U
#define GRAPH_MAX_PPM           60000U

/*
 * 采样队列容量。采样在 TIM3 中断里以精确 25 Hz 产生，主循环异步排空。
 *
 * 容量需覆盖主循环最长的一次阻塞：AI 满窗推理（重采样 + 寻峰 + 12 维特征
 * + 261KB 权重）耗时未知，是这里的主导项；其次是 OLED 刷屏与整帧串口发送。
 * 64 点 = 2.56 秒。若仍溢出，由 queue_overflow 计数经串口上报——绝不静默丢点。
 */
#define SAMPLE_QUEUE_SIZE          64U

/*
 * 启用 TIMING_DEBUG 时，每 25 个采样（约 1 秒）输出一行时基统计文本。
 * 此模式下不再发送二进制波形帧，避免两种格式混在串口里难以解析。
 */
#ifdef TIMING_DEBUG
#define DEBUG_REPORT_SAMPLES       25U
#endif

typedef struct
{
    uint32_t value_q8;
    uint8_t initialized;
} ADC_LowPassFilter;

typedef struct
{
    float    ppm;        /* NDIR 浓度，供曲线显示、AI 累积与串口帧 */
    uint16_t act_mv;     /* ACT 通道毫伏，仅供电平显示（不再进串口帧） */
} SampleItem;

/* ---- 采样队列：ISR 写、主循环读；双方各只写一个索引，无需临界区 ---- */
static volatile SampleItem sample_queue[SAMPLE_QUEUE_SIZE];
static volatile uint16_t   queue_head = 0U;      /* ISR 写入位置 */
static volatile uint16_t   queue_tail = 0U;      /* 主循环读取位置 */
static volatile uint32_t   queue_overflow = 0U;  /* 队列满导致丢弃的采样点数 */

/* ---- 中断与主循环共享的采样状态 ---- */
static volatile ADC_LowPassFilter act_filter = {0U, 0U};
static volatile ADC_LowPassFilter ref_filter = {0U, 0U};
static volatile uint32_t sample_tick = 0U;       /* 累计采样节拍，主循环据此调度 */

/* 供 OLED 显示的最近一次采样快照（撕裂读对显示无害，无需保护） */
static volatile uint16_t    last_act_count = 0U;
static volatile uint16_t    last_ref_count = 0U;
static volatile NDIR_Result last_ndir_result;

static uint16_t concentration_history[HISTORY_LENGTH];
static uint16_t history_count = 0U;
static uint16_t history_write_index = 0U;
static uint32_t concentration_filter_q8 = 0U;
static uint8_t concentration_filter_initialized = 0U;

/*
 * 时基统计的读取接口。统计值本身由 timing.c 维护。
 * 未定义 TIMING_DEBUG 时 Timing_GetStats 不存在，此函数一并省去。
 */
#ifdef TIMING_DEBUG
void App_GetTimingStats(Timing_Stats *out)
{
    Timing_GetStats(out);
}
#endif

/* 前向声明：ADC_FilterValue 定义在下方，而 App_OnSampleTick 需要先用到它。 */
static uint16_t ADC_FilterValue(ADC_LowPassFilter *filter, uint16_t sample);

/* ------------------------------------------------------------------
 * 采样队列
 * ------------------------------------------------------------------ */
static void Queue_Push(const SampleItem *item)
{
    uint16_t next = (uint16_t)((queue_head + 1U) % SAMPLE_QUEUE_SIZE);

    if (next == queue_tail)
    {
        /* 队列满：丢弃并计数。绝不静默丢点——计数会经串口帧上报。 */
        queue_overflow++;
        return;
    }

    sample_queue[queue_head] = *item;
    queue_head = next;
}

static uint8_t Queue_Pop(SampleItem *item)
{
    if (queue_tail == queue_head)
    {
        return 0U;
    }

    *item = sample_queue[queue_tail];
    queue_tail = (uint16_t)((queue_tail + 1U) % SAMPLE_QUEUE_SIZE);
    return 1U;
}

/* ------------------------------------------------------------------
 * 采样节拍回调 —— 由 TIM3 中断以精确 25 Hz 调用
 *
 * 只做"采集 + 滤波 + NDIR 换算 + 入队"。严禁在此调用 OLED / 串口 /
 * AI 推理等耗时操作：中断里耗时越长，主循环越容易被挤压。
 * 实测开销约 1.5 ms（主要是 ADC 的 64 次转换），占 40 ms 周期的 4%。
 * ------------------------------------------------------------------ */
void App_OnSampleTick(void)
{
    SampleItem item;
    uint16_t act_count;
    uint16_t ref_count;
    NDIR_Result result;
#ifdef TIMING_DEBUG
    uint32_t t0 = Timing_Now();
#endif

    /* 记录本次节拍：统计与上一拍的间隔，即真实采样周期。 */
    Timing_MarkTick();

    act_count = ADC_ReadAverage(ADC_ACT_CHANNEL, ADC_AVERAGE_SAMPLES);
    ref_count = ADC_ReadAverage(ADC_REF_CHANNEL, ADC_AVERAGE_SAMPLES);

    act_count = ADC_FilterValue((ADC_LowPassFilter *)&act_filter, act_count);
    ref_count = ADC_FilterValue((ADC_LowPassFilter *)&ref_filter, ref_count);

    NDIR_Calculate(act_count, ref_count, &result);

    /* 发布给主循环显示用。先写数据再增计数，主循环以计数判断新数据已就绪。 */
    last_act_count = act_count;
    last_ref_count = ref_count;
    last_ndir_result = result;

    if (result.valid != 0U)
    {
        item.ppm = result.concentration_ppm;
        item.act_mv = ADC_CountToMillivolt(act_count);
        Queue_Push(&item);
    }

    sample_tick++;

#ifdef TIMING_DEBUG
    /* 统计本次采样回调自身耗时（不含节拍间隔，那个由 Timing_MarkTick 记）。 */
    Timing_RecordIsr(t0);
#endif
}

/*
 * 串口采样率提高到 25 Hz 后，采用 1/8 系数的一阶数字低通。
 * 其实际平滑时间与原来 5 Hz、1/2 系数接近，同时可输出更连续的波形点。
 */
static uint16_t ADC_FilterValue(ADC_LowPassFilter *filter, uint16_t sample)
{
    int32_t difference;

    if (filter->initialized == 0U)
    {
        filter->value_q8 = (uint32_t)sample << 8U;
        filter->initialized = 1U;
    }
    else
    {
        difference = ((int32_t)sample << 8U) - (int32_t)filter->value_q8;
        filter->value_q8 = (uint32_t)((int32_t)filter->value_q8 + difference / 8);
    }

    return (uint16_t)((filter->value_q8 + 128U) >> 8U);
}

/* 对计算后的浓度再做一次低通，避免OLED曲线出现近似竖直的跳变。 */
static uint16_t Concentration_FilterValue(float concentration_ppm)
{
    uint32_t target_q8;
    int32_t difference;

    if (concentration_ppm < 0.0f)
    {
        concentration_ppm = 0.0f;
    }
    else if (concentration_ppm > GRAPH_MAX_PPM)
    {
        concentration_ppm = GRAPH_MAX_PPM;
    }

    target_q8 = (uint32_t)(concentration_ppm * 256.0f + 0.5f);
    if (concentration_filter_initialized == 0U)
    {
        concentration_filter_q8 = target_q8;
        concentration_filter_initialized = 1U;
    }
    else
    {
        difference = (int32_t)target_q8 - (int32_t)concentration_filter_q8;
        /* 1/4滤波：保留呼吸轮廓，同时抑制浓度瞬时跳变。 */
        concentration_filter_q8 = (uint32_t)((int32_t)concentration_filter_q8 +
                                             difference / 4);
    }

    return (uint16_t)((concentration_filter_q8 + 128U) >> 8U);
}

static void History_Add(float concentration_ppm)
{
    uint16_t displayed_value;

    if (concentration_ppm < 0.0f)
    {
        displayed_value = 0U;
    }
    else if (concentration_ppm > NDIR_MAX_DISPLAY_PPM)
    {
        displayed_value = (uint16_t)NDIR_MAX_DISPLAY_PPM;
    }
    else
    {
        displayed_value = (uint16_t)(concentration_ppm + 0.5f);
    }

    concentration_history[history_write_index] = displayed_value;
    history_write_index = (uint16_t)((history_write_index + 1U) % HISTORY_LENGTH);
    if (history_count < HISTORY_LENGTH)
    {
        history_count++;
    }
}

static uint16_t History_Get(uint16_t chronological_index)
{
    uint16_t start_index;

    if (history_count < HISTORY_LENGTH)
    {
        start_index = 0U;
    }
    else
    {
        start_index = history_write_index;
    }

    return concentration_history[(start_index + chronological_index) % HISTORY_LENGTH];
}

/*
 * 曲线采用固定0～60000 ppm纵轴，避免自动缩放把小抖动放大成陡峭跳变。
 * 新数据始终画在屏幕最右侧，历史数据每次刷新向左移动 1 个像素。
 */
static void OLED_DrawConcentrationWaveform(void)
{
    uint16_t index;
    uint16_t value;
    uint32_t graph_span = GRAPH_MAX_PPM - GRAPH_MIN_PPM;
    uint16_t first_x;
    int16_t previous_x = 0;
    int16_t previous_y = GRAPH_BOTTOM;
    int16_t current_x;
    int16_t current_y;

    OLED_DrawLine(0, (int16_t)(GRAPH_TOP - 1U),
                  (int16_t)(OLED_WIDTH - 1U), (int16_t)(GRAPH_TOP - 1U));

    if (history_count < 2U)
    {
        return;
    }

    first_x = (uint16_t)(OLED_WIDTH - history_count);
    for (index = 0U; index < history_count; index++)
    {
        value = History_Get(index);

        if (value > GRAPH_MAX_PPM)
        {
            value = GRAPH_MAX_PPM;
        }

        current_x = (int16_t)(first_x + index);
        current_y = (int16_t)(GRAPH_BOTTOM -
                    (((uint32_t)value - GRAPH_MIN_PPM) *
                     (GRAPH_BOTTOM - GRAPH_TOP) / graph_span));

        if (index != 0U)
        {
            OLED_DrawLine(previous_x, previous_y, current_x, current_y);
        }
        previous_x = current_x;
        previous_y = current_y;
    }
}

static void OLED_DrawMeasurements(uint16_t act_count,
                                  uint16_t ref_count,
                                  const NDIR_Result *result)
{
    uint16_t act_mv = ADC_CountToMillivolt(act_count);
    uint16_t ref_mv = ADC_CountToMillivolt(ref_count);
    uint16_t ratio_x1000;

    OLED_Clear();

    OLED_DrawString(0U, 0U, "A:");
    OLED_DrawUInt(12U, 0U, act_mv, 1U);
    OLED_DrawString(42U, 0U, "mV");
    OLED_DrawString(66U, 0U, "R:");
    OLED_DrawUInt(78U, 0U, ref_mv, 1U);

    OLED_DrawString(0U, 8U, "C:");
    if (result->valid != 0U)
    {
        OLED_DrawUInt(12U,
                      8U,
                      (uint32_t)(result->concentration_ppm + 0.5f),
                      1U);
        OLED_DrawString(48U, 8U, "ppm");

        ratio_x1000 = (uint16_t)(result->ratio * 1000.0f + 0.5f);
        if (ratio_x1000 > 9999U)
        {
            ratio_x1000 = 9999U;
        }
        OLED_DrawString(78U, 8U, "Q:");
        OLED_DrawUInt(90U, 8U, ratio_x1000, 1U);
    }
    else
    {
        OLED_DrawString(12U, 8U, "----");
        OLED_DrawString(48U, 8U, "ppm");
        OLED_DrawString(78U, 8U, "Q:----");
    }

    OLED_DrawConcentrationWaveform();
    OLED_Refresh();
}

int main(void)
{
    SampleItem item;
    uint16_t waveform_samples_ppm[5U];
    uint8_t waveform_sample_index = 0U;
    uint32_t oled_last_tick;
    AI_Status ai_status;
#ifdef TIMING_DEBUG
    uint32_t debug_report_tick;
#endif

    Delay_Init();
    Timing_Init();

    /* PA0/TIM2_CH1：1 Hz、50% 占空比的红外光源调制 PWM。 */
    TIM2_PWM_Init();
    TIM2_PWM_SetDuty(50U);

    /* PA1=ACT 测量通道，PA2=REF 参考通道。 */
    ADC_DualChannel_Init();

    /* PB6=SCL、PB7=SDA，连接 128x64、地址 0x3C 的 SSD1306 OLED。 */
    OLED_Init();

    /* PA9=USART1_TX，以 115200 波特率向串口绘图软件发送测量数据。 */
    USART1_Waveform_Init();

    /* 加载随机森林模型。失败时波形采集与串口照常工作，仅AI字段保持无结果。 */
    (void)AI_Init();

    /* 先取一次显示快照，避免上电后到首次采样之间显示未初始化的数据。 */
    last_ndir_result.valid = 0U;
    last_ndir_result.ratio = 0.0f;
    last_ndir_result.concentration_ppm = 0.0f;
    oled_last_tick = sample_tick;
#ifdef TIMING_DEBUG
    debug_report_tick = sample_tick;
#endif

    /* 最后启动采样定时器：以上外设就绪后才开始产生节拍。 */
    TIM3_SampleTimer_Init();

    while (1)
    {
        /*
         * 排空采样队列——主循环唯一的采样消费点。
         * 采样由 TIM3 中断以精确 25 Hz 产生，这里无论被 OLED、串口还是 AI
         * 推理阻塞多久，中断都会继续填充队列，时间戳始终均匀。
         */
        while (Queue_Pop(&item) != 0U)
        {
            /* OLED绘制滤波后的浓度C，使曲线更接近呼吸波形。 */
            History_Add((float)Concentration_FilterValue(item.ppm));

            /* 累积波形，每满90秒触发一次AI推理（本调用可能耗时较长）。 */
            AI_ProcessSample(item.ppm);

            /*
             * 帧内送的是 ndir_math 算出的浓度（ppm），不是 ACT 电压。
             * 上位机据此可直接得到与端侧同源的浓度，无需再做一套独立标定。
             * NDIR_Calculate 已把值夹在 [0, 50000]，不会溢出 uint16；
             * 此处再夹一次，防止将来改动使该前提失效。
             */
            {
                float ppm_value = item.ppm;

                if (ppm_value < 0.0f)
                {
                    ppm_value = 0.0f;
                }
                else if (ppm_value > 65535.0f)
                {
                    ppm_value = 65535.0f;
                }
                waveform_samples_ppm[waveform_sample_index] =
                    (uint16_t)(ppm_value + 0.5f);
            }
            waveform_sample_index++;
            if (waveform_sample_index >= 5U)
            {
#ifdef TIMING_DEBUG
                /*
                 * 计时模式下不发二进制波形帧：两种格式混在同一条串口上
                 * 无法解析，本阶段只关心时基统计。波形照常采集与显示。
                 */
                waveform_sample_index = 0U;
#else
                /* 每5个40ms采样点组成一帧，整帧长度为64字节。 */
                AI_GetStatus(&ai_status);
                USART1_SendWaveformFrame(waveform_samples_ppm, &ai_status,
                                         queue_overflow);
                waveform_sample_index = 0U;
#endif
            }
        }

        /* OLED按采样节拍刷新（每 OLED_REFRESH_SAMPLES 个点一次）。 */
        if ((uint32_t)(sample_tick - oled_last_tick) >= OLED_REFRESH_SAMPLES)
        {
#ifdef TIMING_DEBUG
            uint32_t oled_t0 = Timing_Now();
#endif

            oled_last_tick = sample_tick;
            OLED_DrawMeasurements(last_act_count,
                                  last_ref_count,
                                  (const NDIR_Result *)&last_ndir_result);

#ifdef TIMING_DEBUG
            Timing_RecordOled(oled_t0);
            Timing_SetOverflow(queue_overflow);
#endif
        }

#ifdef TIMING_DEBUG
        /* 每约 1 秒把统计结果以文本行发出（此模式下不发二进制波形帧）。 */
        {
            uint32_t now_tick = sample_tick;

            if ((uint32_t)(now_tick - debug_report_tick) >= DEBUG_REPORT_SAMPLES)
            {
                Timing_Stats stats;

                debug_report_tick = now_tick;
                App_GetTimingStats(&stats);
                USART1_SendTimingReport(&stats);
            }
        }
#endif
    }
}
