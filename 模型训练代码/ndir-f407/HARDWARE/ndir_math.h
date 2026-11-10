#ifndef __NDIR_MATH_H
#define __NDIR_MATH_H

#include "stm32f4xx.h"

/*
 * 当前样机的临时两点估算参数：
 * 当前空气 Q=1.412，暂按 450 ppm；当前高浓度端 Q=1.000，暂按 50000 ppm。
 * 这些数值不是标准气体标定结果，只能用于样机趋势显示。
 */
#define NDIR_AIR_RATIO             1.412f
#define NDIR_AIR_PPM               450.0f
#define NDIR_BREATH_RATIO          1.000f
#define NDIR_BREATH_PPM          50000.0f

/* 16位历史缓存和串口浓度字段允许的安全显示范围。 */
#define NDIR_MAX_DISPLAY_PPM       50000.0f
#define NDIR_MIN_REFERENCE_COUNT   5U

typedef struct
{
    float ratio;
    float absorbance;
    float concentration_ppm;
    uint8_t valid;
} NDIR_Result;

void NDIR_Calculate(uint16_t act_count,
                    uint16_t ref_count,
                    NDIR_Result *result);

#endif
