#include "ai_bridge.h"
#include "feature_extract.h"
#include "randomforest.h"
#include "randomforest_data.h"
#include "ai_platform.h"

/*
 * ppm -> mmHg 分压换算：mmHg = ppm * 760 / 1e6（标准大气压）。
 *
 * 训练端 CO2 波形单位为 mmHg（VitalDB Primus/CO2 轨道），而本样机 NDIR
 * 输出为 ppm，必须先换算再送特征提取——否则寻峰的 height=12（mmHg）
 * 门限会失配。
 *
 * 注意：该换算的准确性依赖 ndir_math.h 中 NDIR 两点校准参数是否为真值。
 * 当前用的是厂家初值、未经标准气体标定，会直接改变特征 F1/F3/F9/F10 的
 * 绝对量级。正式测量前需用标准气体标定这四个常数。
 */
#define AB_PPM_TO_MMHG_SCALE   0.00076f

/* 满窗判定：90 秒 @ 25Hz */
#define AB_WINDOW_SAMPLES      FE_RAW_WINDOW

/* 呼吸率 = 峰数 / 90 秒 * 60。F9（features[8]）即峰数。
 * 注意与 F11（features[10]）区分：F11 训练端硬编码 60/30，是给模型用的
 * 特征，不是真实呼吸率。 */
#define AB_RESP_RATE_SCALE     (60.0f / 90.0f)

/*
 * X-CUBE-AI 运行所需的激活内存（68 字节）。
 * 本模型启用了 --allocate-inputs/outputs，输入输出缓冲也在这一块之内，
 * 由运行库自行划分，不要再另外分配 data_in / data_out。
 */
AI_ALIGNED(32) static uint8_t ab_pool[AI_RANDOMFOREST_DATA_ACTIVATION_1_SIZE];
static ai_handle ab_activations[] = { ab_pool };

static ai_handle ab_net = AI_HANDLE_NULL;
static ai_buffer *ab_in = NULL;
static ai_buffer *ab_out = NULL;

/* 90 秒波形累积缓冲 */
static float    ab_wave_mmhg[AB_WINDOW_SAMPLES];   /* 2250 * 4B = 9.0 KB */
static float    ab_wave_100hz[FE_WINDOW_SIZE];     /* 9000 * 4B = 36.0 KB */
static float    ab_feats[FE_N_FEATURES];
static uint16_t ab_sample_count = 0U;

static AI_Status ab_status = { 0U, AB_LABEL_NORMAL, 0U };

/* ------------------------------------------------------------------
 * 初始化
 * ------------------------------------------------------------------ */
int AI_Init(void)
{
    ai_error err;
    ai_u16   n_in = 0U;
    ai_u16   n_out = 0U;

    err = ai_randomforest_create_and_init(&ab_net, ab_activations, NULL);
    if (err.type != AI_ERROR_NONE)
    {
        ab_net = AI_HANDLE_NULL;
        return -1;
    }

    ab_in = ai_randomforest_inputs_get(ab_net, &n_in);
    ab_out = ai_randomforest_outputs_get(ab_net, &n_out);
    if ((ab_in == NULL) || (ab_out == NULL) ||
        (n_in < AI_RANDOMFOREST_IN_NUM) || (n_out < AI_RANDOMFOREST_OUT_NUM))
    {
        ab_net = AI_HANDLE_NULL;
        return -1;
    }

    ab_sample_count = 0U;
    ab_status.valid = 0U;
    return 0;
}

/* ------------------------------------------------------------------
 * 单次推理：特征 -> 标签
 * ------------------------------------------------------------------ */
static int AB_RunInference(const float feats[FE_N_FEATURES],
                           uint8_t *label, float *resp_rate)
{
    float  *input_data;
    ai_i32  batch;
    int32_t raw_label;
    int     i;

    if (ab_net == AI_HANDLE_NULL)
    {
        return -1;
    }

    /* 输入张量：f32 (1x1x12) = 48 字节 */
    input_data = (float *)ab_in[0].data;
    if (input_data == NULL)
    {
        return -1;
    }
    for (i = 0; i < FE_N_FEATURES; i++)
    {
        input_data[i] = feats[i];
    }

    batch = ai_randomforest_run(ab_net, ab_in, ab_out);
    if (batch != 1)
    {
        return -1;
    }

    /* 输出 0：int32 类别标签 */
    raw_label = (int32_t)((int32_t *)ab_out[0].data)[0];

    /* 类别越界时按异常处理，避免把非法值传给上位机 */
    if ((raw_label < 0) || (raw_label > (int32_t)AB_LABEL_ABNORMAL))
    {
        raw_label = (int32_t)AB_LABEL_ABNORMAL;
    }
    *label = (uint8_t)raw_label;

    /* 真实呼吸率：峰数 * 60 / 90（不用 F11，见 AB_RESP_RATE_SCALE 注释） */
    *resp_rate = feats[8] * AB_RESP_RATE_SCALE;

    return 0;
}

/* ------------------------------------------------------------------
 * 满窗处理
 * ------------------------------------------------------------------ */
static void AB_ProcessWindow(void)
{
    uint8_t label = AB_LABEL_NORMAL;
    float   resp_rate = 0.0f;
    int     n_resampled;

    /* 25Hz -> 100Hz 线性重采样（2250 -> 9000） */
    n_resampled = FE_Resample25To100(ab_wave_mmhg, AB_WINDOW_SAMPLES,
                                     ab_wave_100hz, FE_WINDOW_SIZE);
    if (n_resampled != FE_WINDOW_SIZE)
    {
        ab_status.valid = 0U;
        return;
    }

    if (FE_Extract(ab_wave_100hz, n_resampled, FE_SAMPLE_RATE, ab_feats) != 0)
    {
        ab_status.valid = 0U;
        return;
    }

    if (AB_RunInference(ab_feats, &label, &resp_rate) != 0)
    {
        ab_status.valid = 0U;
        return;
    }

    /* 呼吸率转 ×10 定点，夹在 uint16 范围内 */
    {
        float scaled = resp_rate * 10.0f;
        uint32_t value;

        if (scaled < 0.0f)
        {
            scaled = 0.0f;
        }
        value = (uint32_t)(scaled + 0.5f);
        if (value > 65535U)
        {
            value = 65535U;
        }
        ab_status.resp_rate_x10 = (uint16_t)value;
    }

    ab_status.label = label;
    ab_status.valid = 1U;
}

/* ------------------------------------------------------------------
 * 对外的采样入口
 * ------------------------------------------------------------------ */
void AI_ProcessSample(float concentration_ppm)
{
    if (ab_sample_count >= AB_WINDOW_SAMPLES)
    {
        return;     /* 防御：正常情况下不会发生 */
    }

    ab_wave_mmhg[ab_sample_count] = concentration_ppm * AB_PPM_TO_MMHG_SCALE;
    ab_sample_count++;

    if (ab_sample_count >= AB_WINDOW_SAMPLES)
    {
        AB_ProcessWindow();
        ab_sample_count = 0U;
    }
}

void AI_GetStatus(AI_Status *status)
{
    if (status == NULL)
    {
        return;
    }
    *status = ab_status;
}
