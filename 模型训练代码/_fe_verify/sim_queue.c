/*
 * 采样队列逻辑仿真：验证环形缓冲在"ISR 持续推入 + 主循环间歇阻塞"下
 * 不丢点、不重复、计数正确。这是纯逻辑测试，不依赖硬件。
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define SAMPLE_QUEUE_SIZE 64U

typedef struct { float ppm; uint16_t act_mv; } SampleItem;

static volatile SampleItem sample_queue[SAMPLE_QUEUE_SIZE];
static volatile uint16_t queue_head = 0U;
static volatile uint16_t queue_tail = 0U;
static volatile uint32_t queue_overflow = 0U;

static void Queue_Push(const SampleItem *item)
{
    uint16_t next = (uint16_t)((queue_head + 1U) % SAMPLE_QUEUE_SIZE);
    if (next == queue_tail) { queue_overflow++; return; }
    sample_queue[queue_head] = *item;
    queue_head = next;
}

static uint8_t Queue_Pop(SampleItem *item)
{
    if (queue_tail == queue_head) return 0U;
    *item = sample_queue[queue_tail];
    queue_tail = (uint16_t)((queue_tail + 1U) % SAMPLE_QUEUE_SIZE);
    return 1U;
}

static void reset(void)
{
    queue_head = queue_tail = 0U;
    queue_overflow = 0U;
}

static int check(const char *name, uint32_t produced, uint32_t consumed,
                 uint32_t expected_overflow)
{
    int ok = (produced == consumed + expected_overflow) &&
             (queue_overflow == expected_overflow);
    printf("  %-34s 产生%5u 消费%5u 丢弃%5u (期望丢弃%5u)  %s\n",
           name, produced, consumed, (unsigned)queue_overflow,
           expected_overflow, ok ? "OK" : "FAIL");
    return ok;
}

int main(void)
{
    SampleItem it = {1.0f, 100U};
    uint32_t produced, consumed;
    int all_ok = 1;
    unsigned i;
    uint32_t n;

    /* 容量 = 63 个可用槽（留一个区分空/满） */
    printf("环形缓冲逻辑测试（容量 %u，可用槽 %u）\n",
           SAMPLE_QUEUE_SIZE, SAMPLE_QUEUE_SIZE - 1);

    /* 1. 稳态：每次产生后立即消费，永不溢出 */
    reset();
    produced = consumed = 0;
    for (n = 0; n < 100000U; n++) {
        it.ppm = (float)n;
        Queue_Push(&it); produced++;
        while (Queue_Pop(&it)) consumed++;
    }
    all_ok &= check("稳态 1:1", produced, consumed, 0);

    /* 2. 恰好填满 63 个，不溢出 */
    reset();
    produced = consumed = 0;
    for (i = 0; i < 63U; i++) { Queue_Push(&it); produced++; }
    while (Queue_Pop(&it)) consumed++;
    all_ok &= check("恰好填满 63", produced, consumed, 0);

    /* 3. 填 64 -> 第 64 个应被丢弃 */
    reset();
    produced = consumed = 0;
    for (i = 0; i < 64U; i++) { Queue_Push(&it); produced++; }
    while (Queue_Pop(&it)) consumed++;
    all_ok &= check("溢出 1 个", produced, consumed, 1);

    /* 4. 长时间阻塞：产生 100 个才消费一次 */
    reset();
    produced = consumed = 0;
    for (n = 0; n < 1000U; n++) {
        it.ppm = (float)n;
        Queue_Push(&it); produced++;
        if ((n % 100U) == 99U) {          /* 每 100 个排空一次 */
            while (Queue_Pop(&it)) consumed++;
        }
    }
    /* 每轮推 100、容量 63 -> 丢 37，共 10 轮 */
    all_ok &= check("阻塞 100 消费一次", produced, consumed, 10U * 37U);

    /* 5. 顺序正确性：FIFO 无乱序、无重复 */
    reset();
    produced = consumed = 0;
    {
        int fifo_ok = 1;
        uint32_t expect = 0;
        for (n = 0; n < 5000U; n++) {
            it.ppm = (float)n;
            Queue_Push(&it); produced++;
            /* 随机节奏消费，模拟主循环被 OLED/推理间歇阻塞 */
            if ((n % 7U) == 0U) {
                while (Queue_Pop(&it)) {
                    if ((uint32_t)it.ppm != expect) { fifo_ok = 0; }
                    expect++;
                    consumed++;
                }
            }
        }
        while (Queue_Pop(&it)) {
            if ((uint32_t)it.ppm != expect) { fifo_ok = 0; }
            expect++; consumed++;
        }
        printf("  %-34s %s (漏丢 %u 点)\n", "FIFO 顺序与完整性",
               fifo_ok ? "OK" : "FAIL", produced - consumed);
        all_ok &= fifo_ok;
    }

    printf("\n%s\n", all_ok ? "全部通过" : "存在失败");
    return all_ok ? 0 : 1;
}
