/*
 * 固件侧的独立运行器：读入 25Hz 波形，走真实固件 C 链路，输出 12 维特征 + 峰数。
 *
 * 供 host_vs_firmware.py / 其他比对脚本调用。
 * 输入文件 _fw_wave.txt：2250 个空格分隔的浮点数（mmHg，25Hz）
 * 输出 stdout：12 个特征值 + 峰数（空格分隔）
 */
#include <stdio.h>
#include "feature_extract.h"

#define MAX_N 9000

static float w25[FE_RAW_WINDOW];
static float w100[MAX_N];
static float feats[FE_N_FEATURES];
static int   peaks[FE_MAX_PEAKS];

int main(void)
{
    FILE *f = fopen("_fw_wave.txt", "r");
    int   i;
    int   n;
    int   np;

    if (f == NULL)
    {
        fprintf(stderr, "no input\n");
        return 1;
    }

    for (i = 0; i < FE_RAW_WINDOW; i++)
    {
        if (fscanf(f, "%f", &w25[i]) != 1)
        {
            fprintf(stderr, "short input at %d\n", i);
            return 1;
        }
    }
    fclose(f);

    n = FE_Resample25To100(w25, FE_RAW_WINDOW, w100, MAX_N);
    if (n != FE_WINDOW_SIZE)
    {
        fprintf(stderr, "RESAMPLE_FAIL n=%d\n", n);
        return 2;
    }

    if (FE_Extract(w100, n, FE_SAMPLE_RATE, feats) != 0)
    {
        fprintf(stderr, "EXTRACT_FAIL\n");
        return 3;
    }

    np = FE_FindBreathPeaks(w100, n, FE_SAMPLE_RATE, peaks, FE_MAX_PEAKS);
    if (np < 0)
    {
        np = 0;
    }

    for (i = 0; i < FE_N_FEATURES; i++)
    {
        printf("%.9g ", (double)feats[i]);
    }
    printf("%d\n", np);

    return 0;
}
