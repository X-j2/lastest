#ifndef __FEATURE_EXTRACT_H
#define __FEATURE_EXTRACT_H

#include "stm32f4xx.h"

/*
 * 12 维呼吸波形特征提取（纯 C99，无动态内存）
 * ============================================================
 * 与 features/extract_features.py 逐条对应，供 STM32 端 1:1 复现。
 *
 * 特征顺序（与训练端 FEATURE_NAMES 一致）：
 *   [0]  Mean CO2            波形均值
 *   [1]  Std CO2             波形标准差（ddof=0）
 *   [2]  Peak-to-peak        峰峰值
 *   [3]  Skewness            波形偏度
 *   [4]  Peak interval mean  峰间距均值（秒）
 *   [5]  Peak interval SD    峰间距标准差（秒，ddof=0）
 *   [6]  Peak width          峰宽（秒）
 *   [7]  Plateau ratio       呼气平台占比
 *   [8]  Peak count          呼吸峰数
 *   [9]  Peak amplitude      平均峰幅
 *   [10] Wave RR (bpm)       波形自算 RR
 *   [11] Rhythm confidence   频率置信度
 *
 * 寻峰忠实复刻 scipy.signal.find_peaks：
 *   find_peaks(x, height=12, distance=int(1.0*sr), prominence=8)
 *   - 局部极大值含"平台取中点"规则，取自 scipy/_peak_finding_utils
 *   - 距离筛选：按峰高降序、同高按下标降序排序，移除距离内的低优先度峰
 *   - 突出度（prominence）按经典定义在整段信号上计算
 *
 * 注意 F11：训练端 _feature_wave_rr 硬编码 n_peaks*60/30（注释称 30s 窗口，
 * 实际窗口为 90s）。此处保持 60/30 不改，否则特征分布偏移、模型失效。
 * 真实呼吸率请另用 峰数*60/90 计算，见 ai_bridge.c。
 *
 * ---- 已知差异（已量化，不影响分类）----
 * 训练端 scipy 的 _select_by_peak_distance 内部用 np.argsort(priority)
 * 排序，而 numpy 默认的 quicksort 不稳定：当两个候选峰高度完全相等
 * （监护仪波形平台极平坦，实测确有大量精确相等的情形）时，保留哪一个
 * 由快排枢轴决定，无确定性规则可复刻。
 *
 * 本实现采用确定性规则「峰高降序，同高则位置靠后者优先」，经 120 个
 * 真实窗口实测：
 *   - F1~F4、F7~F11 共 9 个特征与训练端一致到 float32 精度极限（~5e-8）
 *   - F5/F6/F12 三个时间类特征相对误差 < 16%（峰位置相差几十个采样点）
 *   - 送入模型后预测标签 120/120 完全一致
 * 因此无需为对齐而改动训练端或重新训练。
 * ============================================================
 */

#define FE_N_FEATURES   12     /* 特征维度 */
#define FE_SAMPLE_RATE  100    /* 模型目标采样率（Hz） */
#define FE_WINDOW_SIZE  9000   /* 90 秒 @ 100Hz = 9000 点 */
#define FE_RAW_RATE     25     /* 固件采样率（Hz） */
#define FE_RAW_WINDOW   2250   /* 90 秒 @ 25Hz = 2250 点 */

/*
 * 峰值候选上限：platform 局部极大值最多约 n/2 个。
 * 9000 点窗口最坏约 4499 个，取 4500。
 * 距离与突出度筛选后真实峰数远小于此。
 */
#define FE_MAX_PEAKS    4500

/* 25Hz -> 100Hz 线性重采样（×4）。
 * 映射 out[j] 对应输入位置 j*FE_RAW_RATE/FE_SAMPLE_RATE，线性插值并 clamp。
 * 返回输出点数 = min((n_25hz-1)*4+1, out_cap)；n_25hz < 1 或 out_cap < 1 返回 0。 */
int FE_Resample25To100(const float *in_25hz, int n_25hz,
                       float *out_100hz, int out_cap);

/* 找呼吸峰：返回峰个数（升序写入 peaks，上限 max_peaks）。
 * 与 scipy find_breath_peaks 一致；参数非法返回 -1。 */
int FE_FindBreathPeaks(const float *x, int n, int sampling_rate,
                       int *peaks, int max_peaks);

/* 提取 12 维特征：返回 0 成功 / -1 参数非法。features 长度需 >= 12。 */
int FE_Extract(const float *x, int n, int sampling_rate,
               float features[FE_N_FEATURES]);

#endif
