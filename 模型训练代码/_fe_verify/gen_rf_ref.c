/* 从生成的权重数组里把随机森林参数 dump 成文本，供 Python 重建模型。 */
#include <stdio.h>
#include <stdint.h>
#include "randomforest_data.h"

#define RF_N_TREES      30
#define RF_N_NODES      10424
#define RF_N_CLASSES    4

/* 各数组在 blob 中的字节偏移，取自 randomforest.c 中 g_randomforest_weights_map[0] 的赋值 */
#define OFF_BRANCH_MODE        0U        /* u8   x 30      */
#define OFF_FEATUREIDS         32U       /* u8   x 10424   */
#define OFF_VALUES             10456U    /* f32  x 10424   */
#define OFF_TRUE_NODEIDS       52152U    /* u16  x 10424   */
#define OFF_FALSE_NODEIDS      73000U    /* u16  x 10424   */
#define OFF_CLASS_WEIGHTS      93848U    /* f32  x 20908   */
#define OFF_NB_NODE            177480U   /* u16  x 30      */
#define OFF_CLASS_NODEIDS      177540U   /* u16  x 20908   */
#define OFF_NB_CLASS           219356U   /* u16  x 30      */
#define OFF_CLASSLABELS        219416U   /* s32  x 4       */
#define OFF_BASE_VALUES        219432U   /* f32  x 4       */
#define OFF_CLASS_IDS          219448U   /* u16  x 20908   */

static const uint8_t *g_blob;

static const void *at(uint32_t off)
{
    return (const void *)(g_blob + off);
}

int main(void)
{
    FILE *f = fopen("rf_params.txt", "w");
    int t;
    int i;

    if (f == NULL)
    {
        fprintf(stderr, "cannot open output\n");
        return 1;
    }

    g_blob = (const uint8_t *)s_randomforest_weights_array_u64;

    fprintf(f, "n_trees %d\n", RF_N_TREES);
    fprintf(f, "n_nodes %d\n", RF_N_NODES);
    fprintf(f, "n_classes %d\n", RF_N_CLASSES);

    fprintf(f, "branch_mode");
    for (i = 0; i < RF_N_TREES; i++)
    {
        fprintf(f, " %u", (unsigned)((const uint8_t *)at(OFF_BRANCH_MODE))[i]);
    }
    fprintf(f, "\n");

    fprintf(f, "nb_node");
    for (i = 0; i < RF_N_TREES; i++)
    {
        fprintf(f, " %u", (unsigned)((const uint16_t *)at(OFF_NB_NODE))[i]);
    }
    fprintf(f, "\n");

    fprintf(f, "nb_class");
    for (i = 0; i < RF_N_TREES; i++)
    {
        fprintf(f, " %u", (unsigned)((const uint16_t *)at(OFF_NB_CLASS))[i]);
    }
    fprintf(f, "\n");

    fprintf(f, "classlabels");
    for (i = 0; i < RF_N_CLASSES; i++)
    {
        fprintf(f, " %d", (int)((const int32_t *)at(OFF_CLASSLABELS))[i]);
    }
    fprintf(f, "\n");

    fprintf(f, "base_values");
    for (i = 0; i < RF_N_CLASSES; i++)
    {
        fprintf(f, " %.9g", (double)((const float *)at(OFF_BASE_VALUES))[i]);
    }
    fprintf(f, "\n");

    /* 每节点一个元组，共 10424 行（class_nodeids 是 20908 长的独立数组，不在此） */
    fprintf(f, "nodes_begin\n");
    for (i = 0; i < RF_N_NODES; i++)
    {
        fprintf(f, "%u %u %.9g %u\n",
                (unsigned)((const uint8_t *)at(OFF_FEATUREIDS))[i],
                (unsigned)((const uint16_t *)at(OFF_TRUE_NODEIDS))[i],
                (double)((const float *)at(OFF_VALUES))[i],
                (unsigned)((const uint16_t *)at(OFF_FALSE_NODEIDS))[i]);
    }
    fprintf(f, "nodes_end\n");

    /* 类别权重，共 20908 行 */
    fprintf(f, "weights_begin\n");
    for (i = 0; i < 20908; i++)
    {
        fprintf(f, "%.9g\n", (double)((const float *)at(OFF_CLASS_WEIGHTS))[i]);
    }
    fprintf(f, "weights_end\n");

    /* 类别所属节点号，共 20908 行 */
    fprintf(f, "classnodeids_begin\n");
    for (i = 0; i < 20908; i++)
    {
        fprintf(f, "%u\n", (unsigned)((const uint16_t *)at(OFF_CLASS_NODEIDS))[i]);
    }
    fprintf(f, "classnodeids_end\n");

    /* 每叶所属树号：本模型 class_weights 按树分块，块内按叶子排列。
     * 树 t 的权重块长度为 nb_class[t]。这里直接 dump 每条的树号，
     * 由 class_nodeids + nb_node 反推即可，无需额外数组。 */
    fprintf(f, "nb_node_check");
    for (i = 0; i < RF_N_TREES; i++)
    {
        fprintf(f, " %u", (unsigned)((const uint16_t *)at(OFF_NB_NODE))[i]);
    }
    fprintf(f, "\n");

    fclose(f);
    printf("rf_params.txt written\n");
    return 0;
}
