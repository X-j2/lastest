#ifndef __AI_BRIDGE_H
#define __AI_BRIDGE_H

#include "stm32f4xx.h"

/*
 * AI 推理封装 —— 本模块是对外唯一接口
 * ============================================================
 * main.c 只需调用下面三个函数，不必接触 ai_handle / ai_buffer /
 * 特征数组等内部细节。
 *
 * 内部流程（每次满 90 秒触发一次）：
 *   ppm 浓度累积(2250 点 @25Hz)
 *     -> 换算 mmHg（训练端波形单位为 mmHg）
 *     -> 线性重采样到 9000 点 @100Hz
 *     -> 12 维特征提取
 *     -> X-CUBE-AI 随机森林推理
 *     -> 4 类标签 + 呼吸率
 * ============================================================
 */

/* 分类标签，与训练端 4 类编号一致 */
#define AB_LABEL_NORMAL      0U   /* 正常 */
#define AB_LABEL_TACHYPNEA   1U   /* 急促 */
#define AB_LABEL_SILENT      2U   /* 屏息 */
#define AB_LABEL_ABNORMAL    3U   /* 呼吸异常 */

/* 推理状态，供串口帧填充 */
typedef struct
{
    uint8_t  valid;           /* 1 = 已完成一次完整推理 */
    uint8_t  label;           /* 0..3 */
    uint16_t resp_rate_x10;   /* 呼吸率 × 10 */
} AI_Status;

/* 初始化模型。返回 0 成功，非 0 表示模型创建失败。 */
int AI_Init(void);

/* 送入一个采样点（浓度，单位 ppm）。主循环每次采样调用一次。
 * 内部累积到 90 秒后触发一次完整推理，耗时较长。 */
void AI_ProcessSample(float concentration_ppm);

/* 取当前推理状态（拷贝，不返回内部指针）。 */
void AI_GetStatus(AI_Status *status);

#endif
