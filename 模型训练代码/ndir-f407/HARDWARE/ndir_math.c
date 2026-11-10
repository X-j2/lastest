#include "ndir_math.h"

void NDIR_Calculate(uint16_t act_count,
                    uint16_t ref_count,
                    NDIR_Result *result)
{
    float concentration;

    result->ratio = 0.0f;
    result->absorbance = 0.0f;
    result->concentration_ppm = 0.0f;
    result->valid = 0U;

    if ((ref_count < NDIR_MIN_REFERENCE_COUNT) ||
        (NDIR_BREATH_RATIO == NDIR_AIR_RATIO))
    {
        return;
    }

    /* 先计算双通道比值Q。当前样机实测Q随CO2浓度升高而减小。 */
    result->ratio = (float)act_count / (float)ref_count;
    if (result->ratio <= 0.0f)
    {
        return;
    }

    result->absorbance = result->ratio / NDIR_AIR_RATIO - 1.0f;

    /*
     * 临时两点线性估算公式：
     * C = Cair + (Q-Qair) * (Cbreath-Cair) / (Qbreath-Qair)。
     * 线性换算便于观察当前样机的呼气变化，后续应使用标准气体重新标定。
     */
    concentration = NDIR_AIR_PPM +
                    (result->ratio - NDIR_AIR_RATIO) *
                    (NDIR_BREATH_PPM - NDIR_AIR_PPM) /
                    (NDIR_BREATH_RATIO - NDIR_AIR_RATIO);

    /* 零点附近的噪声可能得到轻微负值，显示时按 0 ppm 处理。 */
    if (concentration < 0.0f)
    {
        concentration = 0.0f;
    }
    else if (concentration > NDIR_MAX_DISPLAY_PPM)
    {
        concentration = NDIR_MAX_DISPLAY_PPM;
    }

    result->concentration_ppm = concentration;
    result->valid = 1U;
}
