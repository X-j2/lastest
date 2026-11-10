# -*- coding: utf-8 -*-
"""生成测试波形 + Python 参考特征值，供 C 实现逐位比对。"""
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from features.extract_features import extract_features, find_breath_peaks

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_WAVES = os.path.join(HERE, 'waves.txt')
OUT_EXPECT = os.path.join(HERE, 'expected.txt')

N = 9000          # 90s @ 100Hz
SR = 100
rng = np.random.default_rng(12345)

cases = []


def synth(rr, amp, baseline=8.0, noise=0.5, n=N, phase0=0.0):
    t = np.arange(n, dtype=float) / SR
    ph = (rr / 60.0 * t + phase0) % 1.0
    w = baseline + amp * np.maximum(0.0, np.sin(2 * np.pi * ph))
    return w + rng.normal(0, noise, n)


# --- 1. 生理性波形，覆盖各类呼吸模式 ---
for rr, amp, tag in [(12, 25, 'normal'), (25, 20, 'tachy'),
                     (8, 30, 'slow'), (4, 28, 'very_slow'),
                     (18, 22, 'borderline'), (30, 35, 'fast_deep'),
                     (10, 15, 'low_amp'), (14, 45, 'high_amp')]:
    cases.append((f'{tag}_rr{rr}', synth(rr, amp)))

# --- 2. 屏息 / 平坦 ---
cases.append(('apnea_flat', np.full(N, 38.0) + rng.normal(0, 0.3, N)))
cases.append(('constant_zero', np.zeros(N)))
cases.append(('constant_neg', np.full(N, -5.0)))

# --- 3. 单峰 / 双峰（峰数边界） ---
w = np.full(N, 8.0); w[4500] = 40.0
cases.append(('single_peak', w))
w = np.full(N, 8.0); w[2000] = 40.0; w[6000] = 42.0
cases.append(('two_peaks', w))

# --- 4. 平台/等值段（验证平台取中点规则） ---
w = np.full(N, 8.0)
w[3000:3010] = 40.0          # 偶数长平台
w[6000:6005] = 38.0          # 奇数长平台
cases.append(('plateaus', w))

# --- 5. 阈值附近（验证 height=12 边界） ---
cases.append(('near_threshold_low', synth(12, 3.5)))   # 峰值≈11.5 < 12
cases.append(('near_threshold_high', synth(12, 4.5)))  # 峰值≈12.5 > 12

# --- 6. 纯噪声 / 方波 ---
cases.append(('noise_only', rng.normal(30, 6, N)))
cases.append(('square_10bpm', 10 + 20 * ((np.arange(N) // 300) % 2)))
cases.append(('square_30bpm', 10 + 20 * ((np.arange(N) // 100) % 2)))

# --- 7. 带直流偏置 / 负值 ---
cases.append(('offset_high', synth(12, 20, baseline=45.0)))
cases.append(('all_negative', synth(12, 20, baseline=-10.0)))

# --- 8. 极小长度（边界，非 9000 点） ---
cases.append(('tiny_n10', np.array([1, 5, 3, 20, 2, 30, 1, 25, 4, 40], dtype=float)))
cases.append(('n3', np.array([0.0, 5.0, 0.0])))

# --- 9. 真实数据（若本地有测试样本则一并覆盖） ---
real_dir = os.path.join(os.path.dirname(HERE), 'test_samples')
if os.path.isdir(real_dir):
    import glob
    for path in sorted(glob.glob(os.path.join(real_dir, '*.csv')))[:6]:
        arr = np.loadtxt(path, delimiter=',', skiprows=1)
        if arr.ndim == 2 and arr.shape[1] >= 2:
            arr = arr[:, 1]
        arr = np.asarray(arr, dtype=float).ravel()
        if len(arr) >= N:
            cases.append((f'real_{os.path.basename(path)[:-4]}', arr[:N]))

# ---- 输出 ----
with open(OUT_WAVES, 'w', encoding='ascii') as fw, \
     open(OUT_EXPECT, 'w', encoding='ascii') as fe:
    fw.write(f'{len(cases)}\n')
    for name, w in cases:
        fw.write(f'{name} {len(w)}\n')
        fw.write(' '.join(f'{float(v):.9g}' for v in w) + '\n')
        feat = extract_features(w, SR)
        pk = find_breath_peaks(w, SR)
        fe.write(f'{name} peaks={len(pk)}\n')
        fe.write(' '.join(f'{float(v):.17g}' for v in feat) + '\n')

print(f'生成 {len(cases)} 个用例')
for name, w in cases:
    print(f'  {name:24s} n={len(w)}')
