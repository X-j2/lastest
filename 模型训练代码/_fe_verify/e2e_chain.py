# -*- coding: utf-8 -*-
"""完整链路仿真：对比「理想 100Hz」与「固件 25Hz 链路」的预测标签。

固件链路（ai_bridge.c）：
    ppm(25Hz) --x0.00076--> mmHg(25Hz) --线性插值--> mmHg(100Hz) --> 12维特征 --> 模型

理想链路（训练端）：
    mmHg(100Hz) --> 12维特征 --> 模型

二者差异来自：25Hz 采样损失 + 线性插值引入的波形畸变。
本脚本量化这个差异，用于评估端侧部署对分类精度的影响。
"""
import os
import sys
import subprocess
import numpy as np
import onnxruntime as ort

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

PPM_TO_MMHG = 0.00076
MMHG_TO_PPM = 1.0 / PPM_TO_MMHG


def resample_25_to_100(in25):
    """与 feature_extract.c 的 FE_Resample25To100 完全一致的实现。"""
    n = len(in25)
    out = np.empty(9000, dtype=np.float64)
    for j in range(9000):
        sp = j * 25.0 / 100.0
        bi = int(sp)
        frac = sp - bi
        if bi >= n - 1:
            out[j] = in25[n - 1]
        else:
            out[j] = in25[bi] + frac * (in25[bi + 1] - in25[bi])
    return out


def load_cases(path):
    """读 waves.txt"""
    lines = open(path).read().split('\n')
    cases, i = [], 0
    while i < len(lines):
        t = lines[i].split()
        if len(t) != 2:
            i += 1
            continue
        n = int(t[1])
        cases.append((t[0], np.array([float(v) for v in lines[i + 1].split()][:n])))
        i += 2
    return cases


def main():
    from features.extract_features import extract_features

    cases = load_cases(os.path.join(HERE, 'waves.txt'))
    sess = ort.InferenceSession(os.path.join(ROOT, 'etco2_model_v2.onnx'),
                                providers=['CPUExecutionProvider'])
    iname = sess.get_inputs()[0].name

    def predict(feats):
        X = np.array([feats], dtype=np.float32)
        return int(np.asarray(sess.run(None, {iname: X})[0]).ravel()[0])

    lab_ideal, lab_fw = [], []
    for name, x100 in cases:
        # 理想：直接用 100Hz mmHg 波形
        f_ideal = extract_features(x100, 100)
        lab_ideal.append(predict(f_ideal))

        # 固件：先降采样到 25Hz（模拟传感器以 25Hz 采样），
        # 再转 ppm，再走固件的换算+重采样链路
        x25_mmhg = x100[::4]                       # 2250 点
        ppm25 = x25_mmhg * MMHG_TO_PPM             # 传感器输出单位
        back_mmhg25 = ppm25 * PPM_TO_MMHG          # ai_bridge.c 的第一步
        back_mmhg100 = resample_25_to_100(back_mmhg25)
        f_fw = extract_features(back_mmhg100, 100)
        lab_fw.append(predict(f_fw))

    lab_ideal = np.array(lab_ideal)
    lab_fw = np.array(lab_fw)
    agree = int((lab_ideal == lab_fw).sum())

    print(f'窗口数 {len(cases)}')
    print(f'理想 100Hz 链路 vs 固件 25Hz 链路，标签一致: {agree}/{len(cases)} = {agree/len(cases)*100:.1f}%')
    print()
    import collections
    print('理想链路标签分布:', dict(sorted(collections.Counter(lab_ideal.tolist()).items())))
    print('固件链路标签分布:', dict(sorted(collections.Counter(lab_fw.tolist()).items())))
    print()
    if agree < len(cases):
        print('不一致明细:')
        for i, (name, _) in enumerate(cases):
            if lab_ideal[i] != lab_fw[i]:
                print(f'   {name}: 理想={lab_ideal[i]}  固件={lab_fw[i]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
