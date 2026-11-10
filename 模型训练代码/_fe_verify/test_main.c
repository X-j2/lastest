/* 主机端测试驱动：读取 waves.txt，逐用例调用 FE_Extract，输出到 c_out.txt。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "feature_extract.h"

#define MAX_N 9000

static float wave[MAX_N];
static float feats[FE_N_FEATURES];
static int   peaks[FE_MAX_PEAKS];

int main(int argc, char **argv)
{
    FILE *fw;
    FILE *fo;
    FILE *fpk;
    char  line[256];
    int   n_cases = 0;
    int   c;
    int   i;

    (void)argc;
    (void)argv;

    fw = fopen("waves.txt", "r");
    fo = fopen("c_out.txt", "w");
    fpk = fopen("c_peaks.txt", "w");
    if ((fw == NULL) || (fo == NULL))
    {
        fprintf(stderr, "open failed\n");
        return 1;
    }

    if (fscanf(fw, "%d", &n_cases) != 1)
    {
        fprintf(stderr, "bad header\n");
        return 1;
    }

    for (c = 0; c < n_cases; c++)
    {
        char name[128];
        int  n = 0;
        int  pk;

        if (fscanf(fw, "%127s %d", name, &n) != 2)
        {
            fprintf(stderr, "bad case header at %d\n", c);
            return 1;
        }
        if (n > MAX_N)
        {
            fprintf(stderr, "n too large: %d\n", n);
            return 1;
        }

        for (i = 0; i < n; i++)
        {
            if (fscanf(fw, "%f", &wave[i]) != 1)
            {
                fprintf(stderr, "bad sample %d of %s\n", i, name);
                return 1;
            }
        }

        /* 消费该行剩余字符（波形数据后可能残留换行） */
        if (fgets(line, sizeof(line), fw) == NULL) { /* EOF 正常 */ }

        memset(feats, 0, sizeof(feats));
        if (FE_Extract(wave, n, FE_SAMPLE_RATE, feats) != 0)
        {
            fprintf(stderr, "FE_Extract failed for %s\n", name);
            return 1;
        }

        pk = FE_FindBreathPeaks(wave, n, FE_SAMPLE_RATE, peaks, FE_MAX_PEAKS);
        if (pk < 0)
        {
            pk = 0;
        }

        if (fpk != NULL)
        {
            fprintf(fpk, "%s", name);
            for (i = 0; i < pk; i++)
            {
                fprintf(fpk, " %d", peaks[i]);
            }
            fprintf(fpk, "\n");
        }

        fprintf(fo, "%s peaks=%d\n", name, pk);
        for (i = 0; i < FE_N_FEATURES; i++)
        {
            fprintf(fo, "%.17g ", (double)feats[i]);
        }
        fprintf(fo, "\n");
    }

    fclose(fw);
    fclose(fo);
    if (fpk != NULL) { fclose(fpk); }
    printf("done: %d cases\n", n_cases);
    return 0;
}
