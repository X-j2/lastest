# 端侧部署验证脚本

用于验证 `ndir-f407/AI/` 里的 C 实现与训练端 Python / ONNX 模型的一致性。
**全部在 PC 上运行，不需要开发板。**

## 脚本说明

| 脚本 | 作用 |
|---|---|
| `gen_cases.py` | 生成 23 个**合成**测试波形（平台、阈值边界、平坦、方波、极小长度等）+ Python 参考特征 |
| `gen_real.py` | 生成 20 个**真实**窗口（从 `cache/vitaldb/` 按峰数分桶各取 4 个）|
| `gen_big.py` | 生成 120 个**真实**窗口，用于量化统计（推荐用这个）|
| `test_main.c` | C 侧驱动：读 `waves.txt`，调 `FE_Extract`，输出 `c_out.txt`（含峰序列 `c_peaks.txt`）|
| `compare.py` | 比对 `expected.txt`（Python）与 `c_out.txt`（C）|
| `gen_rf_ref.c` | 从固件权重数组 `s_randomforest_weights_array_u64` 导出随机森林参数到 `rf_params.txt` |
| `rf_rebuild.py` | **三方交叉验证**：C 权重 vs ONNX 逐元素比对 + 只用 C 参数遍历 vs onnxruntime |
| `e2e_chain.py` | **完整链路仿真**：理想 100Hz 路径 vs 固件 25Hz 路径（降采样→ppm→换算→重采样）的标签一致性 |
| `fw_runner.c` | 固件侧独立运行器：读 25Hz 波形，走真实 C 链路，输出 12 维特征 + 峰数 |
| `host_vs_firmware.py` | **上位机一致性验证**：AST 提取 `上位机代码/co2_monitor.py` 的特征函数，与固件 C 逐位比对 |
| `stm32f4xx_stub.h` | 主机端编译用的桩头文件（只提供 `stdint.h`）|

## 用法

```bash
cd _fe_verify

# 1. 特征提取一致性（120 个真实窗口）
python gen_big.py
gcc -O0 -I. -I../ndir-f407/AI test_main.c ../ndir-f407/AI/feature_extract.c -lm -o test.exe
./test.exe
python compare.py

# 2. 推理链路三方验证
gcc -O0 -std=c99 -I../ndir-f407/AI/xcube_ai/App -I../ndir-f407/AI/xcube_ai/Inc \
    gen_rf_ref.c ../ndir-f407/AI/xcube_ai/App/randomforest_data_params.c -o gen_rf_ref.exe
./gen_rf_ref.exe
python rf_rebuild.py

# 3. 完整链路仿真
python e2e_chain.py

# 4. 上位机特征实现 vs 固件 C（需要 ../../上位机代码/co2_monitor.py）
python host_vs_firmware.py
```

## 实测结论（2026-09）

| 检查 | 结果 |
|---|---|
| 合成用例特征比对 | 23/23 一致 |
| 真实窗口特征比对（120 个）| 9 个特征一致到 float32 极限（~5e-8）；F5/F6/F12 因浮点平局偏差 <16% |
| 上述差异对模型输出的影响 | 预测标签 **120/120 一致** |
| C 权重 vs ONNX（6 个数组）| 逐元素一致 |
| C 参数遍历 vs onnxruntime | 标签 **120/120 一致** |
| 完整链路（理想 100Hz vs 固件 25Hz）| 标签 **120/120 一致** |
| 上位机特征实现 vs 固件 C（60 窗口）| 峰数 0/60 不一致、标签 **60/60 一致**；F6/F12 有 <31% 偏差 |
| 重采样 + 特征提取耗时 | x86 0.08 ms/窗口（Cortex-M4 估算 <5 ms）|

## 2026-09 修复：重采样返回点数不足（致命）

`FE_Resample25To100` 原按 `(n-1)×4+1` 计算输出点数，对 2250 点输入返回 **8997**，
而 `ai_bridge.c` 要求满窗恰好 9000 点：

```c
if (n_resampled != FE_WINDOW_SIZE) { ab_status.valid = 0U; return; }
```

后果：**端侧 AI 永远不会产生结果**，`frame[11]` 恒为 `0x00`，上位机只能回退到
本地推理。且全程不报错——正是"静默失效"的典型形态。

**修复**：输出点数改为 `n × 4 = 9000`，映射仍为 `src = j/4`（正确的 100Hz 时间基，
不能改成按端点归一化——那会引入随 j 累积的漂移，2250 点下末尾偏差达 0.67 个采样点）。
多出的最后 3 点对应源位置已越过末点，按保持处理。

**验证**：修复后 j=0..8996 与旧实现**逐位一致**（最大差异 0.000000），
120 个真实窗口用真实固件 C 代码跑完整链路，**0 个重采样失败、标签 120/120**。

## 已知差异说明

`compare.py` 会在 F5/F6/F12 上报告偏差，根因是：

训练端 scipy 的 `_select_by_peak_distance` 内部用 `np.argsort(priority)`，
而 numpy 默认 quicksort **不稳定**——两个候选峰高度**精确相等**时（监护仪波形
平坦平台，实测大量存在），保留哪一个由排序算法的枢轴决定，**无确定性规则可复刻**。

C 端采用确定性规则「峰高降序，同高则位置靠后者优先」。已实测该差异不影响
模型输出（120/120 标签一致），因此无需改动训练端或重新训练。

详见 `ndir-f407/AI/feature_extract.h` 头部注释。
