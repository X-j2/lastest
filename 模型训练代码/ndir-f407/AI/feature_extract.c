#include <math.h>
#include <stddef.h>
#include "feature_extract.h"

/*
 * 寻峰参数，与 features/extract_features.py 的 find_breath_peaks 一致：
 *   find_peaks(x, height=12, distance=int(1.0*sampling_rate), prominence=8)
 */
#define FE_PEAK_HEIGHT      12.0f   /* 峰值高度门限（mmHg） */
#define FE_PEAK_PROMINENCE   8.0f   /* 突出度门限（mmHg） */
#define FE_WIDTH_HALF_HEIGHT 7.5f   /* F7 峰宽半高：固定 7.5 mmHg */
#define FE_PLATEAU_TOLERANCE 3.0f   /* F8 平台容差：固定 3.0 mmHg */

/*
 * 中间缓冲全部放静态区，不放栈（本工程栈仅 1KB 量级）。
 *
 * 容量依据：n 点序列的局部极大值最多约 n/2 个，n = 9000 时为 4499。
 * 实测含噪波形（9000 点）局部极大值约 2900~3000 个，必须按最坏情况
 * 预留，否则截断会静默丢掉真实呼吸峰。
 *
 * 峰下标用 uint16 存放（9000 < 65536），两个数组共 9KB + 9KB = 18KB。
 */
#define FE_MAX_CANDIDATES   FE_MAX_PEAKS   /* 4500，覆盖 n/2 最坏情况 */

static uint16_t s_cand[FE_MAX_CANDIDATES];   /* 候选峰下标（排序时被就地打乱） */
static uint16_t s_kept[FE_MAX_CANDIDATES];   /* 筛选后的峰，按位置升序 */
static int      s_kept_count = 0;

/* ------------------------------------------------------------------
 * 局部极大值（含平台取中点），复刻 scipy _local_maxima_1d
 *
 * 平台 [i_a, i_b] 取中点为 (i_a + i_b) / 2（整除，向下取整），
 * 且要求平台两侧都有严格下降沿——首尾处的平台不算峰。
 * height 门限在此一并施加：height 是逐峰独立属性，先取极大值再过滤
 * 与合并处理完全等价。
 * ------------------------------------------------------------------ */
static int FE_LocalMaxima(const float *x, int n, uint16_t *out, int max_out)
{
    int index = 1;
    int count = 0;

    while (index < (n - 1))
    {
        if (x[index - 1] < x[index])
        {
            int i_a = index;
            int i_b = index;

            /* 向右扩展等值平台 */
            while ((i_b < (n - 1)) && (x[i_b + 1] == x[i_a]))
            {
                i_b++;
            }

            /* 平台右端必须有严格下降沿，且峰值过 height 门限 */
            if ((i_b < (n - 1)) && (x[i_b + 1] < x[i_a]) && (x[i_a] >= FE_PEAK_HEIGHT))
            {
                if (count < max_out)
                {
                    out[count] = (uint16_t)((i_a + i_b) / 2);
                    count++;
                }
            }

            index = i_b + 1;
        }
        else
        {
            index++;
        }
    }

    return count;
}

/* a 的优先度是否低于 b：峰高降序，同高则下标降序（等价于先出现的优先）。 */
static int FE_LowerPriority(const float *x, int a, int b)
{
    if (x[a] < x[b])
    {
        return 1;
    }
    if (x[a] > x[b])
    {
        return 0;
    }
    return (a < b) ? 1 : 0;
}

/* 小顶堆下沉：堆序键为「优先度」，根为优先度最低者。 */
static void FE_SiftDown(uint16_t *a, int root, int end, const float *x)
{
    while ((root * 2 + 1) <= end)
    {
        int child = root * 2 + 1;

        if (((child + 1) <= end) && (FE_LowerPriority(x, a[child + 1], a[child]) != 0))
        {
            child = child + 1;      /* 取优先度更低的子节点 */
        }

        if (FE_LowerPriority(x, a[child], a[root]) != 0)
        {
            uint16_t tmp = a[root];
            a[root] = a[child];
            a[child] = tmp;
            root = child;
        }
        else
        {
            break;
        }
    }
}

/* 堆排序：结果按优先度降序排列，a[0] 优先度最高。O(m log m)。 */
static void FE_SortByPriority(uint16_t *a, int m, const float *x)
{
    int start;
    int end;

    if (m < 2)
    {
        return;
    }

    for (start = (m - 2) / 2; start >= 0; start--)
    {
        FE_SiftDown(a, start, m - 1, x);
    }

    for (end = m - 1; end > 0; end--)
    {
        uint16_t tmp = a[0];
        a[0] = a[end];
        a[end] = tmp;
        FE_SiftDown(a, 0, end - 1, x);
    }
}

/* ------------------------------------------------------------------
 * 峰间距筛选，复刻 scipy _select_by_peak_distance：
 *
 * 按优先度（峰高降序、同高按下标降序）从高到低处理；当前峰只要与
 * 任一「已被保留」的峰相距 < distance 个采样点，就丢弃。
 *
 * 注意是「只按已保留峰判断」而非传播删除——已被丢弃的峰不会再去
 * 否决后面的峰（已用 scipy 实测确认：峰序列 1/6/11、距离 6 时结果为
 * [1, 11]，而非 [1]）。
 *
 * 保留峰两两间距 >= distance，数量上限约 n/distance（9000/100 = 90），
 * 因此用「按位置升序的保留表 + 二分查找」实现，复杂度 O(m log m)，
 * 避免朴素 O(m^2) 在 m≈3000 时超出推理时间预算。
 * ------------------------------------------------------------------ */
static void FE_SelectByDistance(const float *x, int m, int distance)
{
    int i;

    FE_SortByPriority(s_cand, m, x);

    s_kept_count = 0;
    if (distance < 1)
    {
        distance = 1;
    }

    for (i = 0; i < m; i++)
    {
        int p = (int)s_cand[i];
        int lo = 0;
        int hi = s_kept_count;
        int pos;
        int insert;
        int j;

        /* 二分查找 p 在保留表中的插入位置（保留表按位置升序） */
        while (lo < hi)
        {
            int mid = (lo + hi) / 2;

            if ((int)s_kept[mid] < p)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        pos = lo;

        /* 任一相邻保留峰落在 distance 之内即丢弃（严格小于） */
        insert = 1;
        if ((pos < s_kept_count) && (((int)s_kept[pos] - p) < distance))
        {
            insert = 0;
        }
        if ((pos > 0) && ((p - (int)s_kept[pos - 1]) < distance))
        {
            insert = 0;
        }

        if (insert != 0)
        {
            for (j = s_kept_count; j > pos; j--)
            {
                s_kept[j] = s_kept[j - 1];
            }
            s_kept[pos] = (uint16_t)p;
            s_kept_count++;
        }
    }
}

/* ------------------------------------------------------------------
 * 突出度（prominence），复刻 scipy _peak_prominences（wlen = 整段信号）。
 * 向两侧扩展直到遇到高于峰值的样本，两侧各自的最低点为左/右基底，
 * 突出度 = 峰值 - max(左基底, 右基底)。
 * ------------------------------------------------------------------ */
static float FE_Prominence(const float *x, int n, int peak)
{
    float peak_val = x[peak];
    float left_min = peak_val;
    float right_min = peak_val;
    int i;

    for (i = peak; (i >= 0) && (x[i] <= peak_val); i--)
    {
        if (x[i] < left_min)
        {
            left_min = x[i];
        }
    }

    for (i = peak; (i < n) && (x[i] <= peak_val); i++)
    {
        if (x[i] < right_min)
        {
            right_min = x[i];
        }
    }

    return peak_val - ((left_min > right_min) ? left_min : right_min);
}

/* ------------------------------------------------------------------
 * 完整筛选链：局部极大值 -> height -> distance -> prominence
 * 结果写入 s_kept[0..s_kept_count)，按位置升序（与 scipy 返回顺序一致）。
 * ------------------------------------------------------------------ */
static int FE_FilterPeaks(const float *x, int n, int sampling_rate)
{
    int m;
    int out;
    int i;

    s_kept_count = 0;

    m = FE_LocalMaxima(x, n, s_cand, FE_MAX_CANDIDATES);
    if (m < 1)
    {
        return 0;
    }

    FE_SelectByDistance(x, m, (int)(1.0f * (float)sampling_rate));

    /* 突出度过滤，就地压缩（out <= i，原地写安全） */
    out = 0;
    for (i = 0; i < s_kept_count; i++)
    {
        if (FE_Prominence(x, n, (int)s_kept[i]) >= FE_PEAK_PROMINENCE)
        {
            s_kept[out] = s_kept[i];
            out++;
        }
    }
    s_kept_count = out;

    return s_kept_count;
}

/* ------------------------------------------------------------------
 * 25Hz -> 100Hz 线性重采样
 *
 * 时间基准：25 Hz 采样点间隔 40 ms，100 Hz 采样点间隔 10 ms，故输出第 j 点
 * 对应输入位置 j×10/40 = j/4。这是唯一的正确映射，不能改成按端点归一化
 * ——归一化会让映射随 j 累积漂移（2250 点输入下末尾偏差达 0.67 个采样点）。
 *
 * 输出点数取 n_25hz × 4 = 9000，而不是 (n-1)×4+1 = 8997。
 *
 * 为什么：ai_bridge.c 要求满窗恰好 9000 点，8997 会让整个窗口静默作废
 * （详见该文件 AB_ProcessWindow 的返回值校验）。2250 个输入点覆盖 90 秒，
 * 100 Hz 下就应是 9000 点。
 *
 * j = 0..8996 与旧实现逐位一致；多出的 j = 8997..8999 对应源位置
 * 2249.25/2249.5/2249.75，已越过最后一点，按保持处理（复制 in[n-1]），
 * 等价于末尾 30 ms 的零阶保持，不产生外插过冲。
 * ------------------------------------------------------------------ */
int FE_Resample25To100(const float *in_25hz, int n_25hz,
                       float *out_100hz, int out_cap)
{
    int out_count;
    int j;

    if ((in_25hz == NULL) || (out_100hz == NULL) || (n_25hz < 1) || (out_cap < 1))
    {
        return 0;
    }

    /* 目标点数 = 输入点数 × 倍率，并受输出容量限制 */
    out_count = n_25hz * (FE_SAMPLE_RATE / FE_RAW_RATE);
    if (out_count > out_cap)
    {
        out_count = out_cap;
    }

    if (n_25hz == 1)
    {
        out_100hz[0] = in_25hz[0];
        return 1;
    }

    for (j = 0; j < out_count; j++)
    {
        /* 源位置 = j × 25 / 100 = j / 4 */
        float source_pos = (float)j * (float)FE_RAW_RATE / (float)FE_SAMPLE_RATE;
        int   base_index = (int)source_pos;
        float frac;

        if (base_index >= (n_25hz - 1))
        {
            /* 越过最后一个输入点：保持，不做外插 */
            out_100hz[j] = in_25hz[n_25hz - 1];
            continue;
        }

        frac = source_pos - (float)base_index;
        out_100hz[j] = in_25hz[base_index] +
                       frac * (in_25hz[base_index + 1] - in_25hz[base_index]);
    }

    return out_count;
}

/* ------------------------------------------------------------------
 * 找呼吸峰（对外接口）
 * ------------------------------------------------------------------ */
int FE_FindBreathPeaks(const float *x, int n, int sampling_rate,
                       int *peaks, int max_peaks)
{
    int i;
    int out_count;

    if ((x == NULL) || (peaks == NULL) || (n < 3) || (sampling_rate <= 0) ||
        (max_peaks < 1) || (n > (2 * FE_MAX_CANDIDATES)))
    {
        return -1;
    }

    FE_FilterPeaks(x, n, sampling_rate);

    out_count = 0;
    for (i = 0; (i < s_kept_count) && (out_count < max_peaks); i++)
    {
        peaks[out_count] = (int)s_kept[i];
        out_count++;
    }

    return out_count;
}

/* ------------------------------------------------------------------
 * 12 维特征提取
 * ------------------------------------------------------------------ */
int FE_Extract(const float *x, int n, int sampling_rate,
               float features[FE_N_FEATURES])
{
    int    peak_count;
    const uint16_t *peaks;
    double sum = 0.0;
    double mean;
    double var = 0.0;
    double std_dev;
    float  min_val;
    float  max_val;
    int    i;

    if ((x == NULL) || (features == NULL) || (n < 1) || (sampling_rate <= 0) ||
        (n > (2 * FE_MAX_CANDIDATES)))
    {
        return -1;
    }

    peak_count = FE_FilterPeaks(x, n, sampling_rate);
    peaks = s_kept;     /* 按位置升序，后续特征直接复用 */

    /* ---- F1-F4 基础统计 ---- */
    min_val = x[0];
    max_val = x[0];
    for (i = 0; i < n; i++)
    {
        sum += (double)x[i];
        if (x[i] < min_val) { min_val = x[i]; }
        if (x[i] > max_val) { max_val = x[i]; }
    }
    mean = sum / (double)n;

    for (i = 0; i < n; i++)
    {
        double d = (double)x[i] - mean;
        var += d * d;
    }
    var /= (double)n;               /* ddof = 0 */
    std_dev = sqrt(var);

    features[0] = (float)mean;                    /* F1 均值 */
    features[1] = (float)std_dev;                 /* F2 标准差 */
    features[2] = max_val - min_val;              /* F3 峰峰值 */

    if (std_dev > 1e-6)                           /* F4 偏度（三阶标准矩） */
    {
        double moment3 = 0.0;

        for (i = 0; i < n; i++)
        {
            double d = (double)x[i] - mean;
            moment3 += d * d * d;
        }
        moment3 /= (double)n;
        features[3] = (float)(moment3 / (std_dev * std_dev * std_dev));
    }
    else
    {
        features[3] = 0.0f;
    }

    /* ---- F5-F6 峰间距（秒，ddof = 0） ---- */
    if (peak_count >= 2)
    {
        double interval_sum = 0.0;
        double interval_mean;
        double interval_var = 0.0;
        int    interval_count = peak_count - 1;

        for (i = 1; i < peak_count; i++)
        {
            interval_sum += (double)((int)peaks[i] - (int)peaks[i - 1]) / (double)sampling_rate;
        }
        interval_mean = interval_sum / (double)interval_count;

        for (i = 1; i < peak_count; i++)
        {
            double d = ((double)((int)peaks[i] - (int)peaks[i - 1]) / (double)sampling_rate)
                       - interval_mean;
            interval_var += d * d;
        }
        interval_var /= (double)interval_count;

        features[4] = (float)interval_mean;
        features[5] = (float)sqrt(interval_var);
    }
    else
    {
        features[4] = 0.0f;
        features[5] = 0.0f;
    }

    /* ---- F7 峰宽（秒） ---- */
    if (peak_count > 0)
    {
        double width_sum = 0.0;

        for (i = 0; i < peak_count; i++)
        {
            int   p = (int)peaks[i];
            float threshold = x[p] - FE_WIDTH_HALF_HEIGHT;
            int   left = p;
            int   right = p;

            while ((left > 0) && (x[left] > threshold))
            {
                left--;
            }
            while ((right < (n - 1)) && (x[right] > threshold))
            {
                right++;
            }

            width_sum += (double)(right - left) / (double)sampling_rate;
        }

        features[6] = (float)(width_sum / (double)peak_count);
    }
    else
    {
        features[6] = 0.0f;
    }

    /* ---- F8 呼气平台占比 ---- */
    if ((peak_count > 0) && (max_val > 0.0f))
    {
        float threshold = max_val - FE_PLATEAU_TOLERANCE;
        int   plateau_points = 0;

        for (i = 0; i < n; i++)
        {
            if (x[i] >= threshold)
            {
                plateau_points++;
            }
        }

        features[7] = (float)plateau_points / (float)n;
    }
    else
    {
        features[7] = 0.0f;
    }

    /* ---- F9-F10 峰数 / 平均峰幅 ---- */
    features[8] = (float)peak_count;
    if (peak_count > 0)
    {
        double amp_sum = 0.0;

        for (i = 0; i < peak_count; i++)
        {
            amp_sum += (double)x[peaks[i]] - (double)min_val;
        }
        features[9] = (float)(amp_sum / (double)peak_count);
    }
    else
    {
        features[9] = 0.0f;
    }

    /* ---- F11 波形自算 RR ----
     * 训练端硬编码 n_peaks*60/30（尽管窗口实为 90 秒）。此处必须保持
     * 60/30 不变，否则特征分布偏移、已训练模型失效。 */
    if (peak_count > 0)
    {
        features[10] = (float)peak_count * 60.0f / 30.0f;
    }
    else
    {
        features[10] = 0.0f;
    }

    /* ---- F12 频率置信度（用原始采样点距，不除以采样率） ---- */
    if (peak_count >= 3)
    {
        double interval_sum = 0.0;
        double interval_mean;
        double interval_var = 0.0;
        double cv;
        int    interval_count = peak_count - 1;

        for (i = 1; i < peak_count; i++)
        {
            interval_sum += (double)((int)peaks[i] - (int)peaks[i - 1]);
        }
        interval_mean = interval_sum / (double)interval_count;

        if (interval_mean < 1e-6)
        {
            features[11] = 0.0f;
        }
        else
        {
            for (i = 1; i < peak_count; i++)
            {
                double d = (double)((int)peaks[i] - (int)peaks[i - 1]) - interval_mean;
                interval_var += d * d;
            }
            interval_var /= (double)interval_count;

            cv = sqrt(interval_var) / interval_mean;
            features[11] = (cv < 1.0) ? (float)(1.0 - cv) : 0.0f;
        }
    }
    else
    {
        features[11] = 0.0f;
    }

    return 0;
}
