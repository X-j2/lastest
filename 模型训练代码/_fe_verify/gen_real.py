# -*- coding: utf-8 -*-
"""用缓存的 VitalDB 真实波形生成测试集：取若干 90s 窗口，含各类呼吸模式。"""
import os
import sys
import glob
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

from features.extract_features import extract_features, find_breath_peaks
from preprocessing.signal_processing import fill_short_gaps, detect_outliers

N = 9000
SR = 100
CACHE = os.path.join(ROOT, 'cache', 'vitaldb')

files = sorted(glob.glob(os.path.join(CACHE, '*.npz')))
print(f'缓存病例 {len(files)} 个')

# 尽量挑选峰数分布不同的窗口，覆盖各呼吸模式
buckets = {'p0': [], 'p1_5': [], 'p6_15': [], 'p16_30': [], 'p31plus': []}
rng = np.random.default_rng(7)

for path in files:
    data = np.load(path)
    co2 = np.asarray(data['co2'], dtype=float)
    if len(co2) < N:
        continue

    # 随机起点，最多取 3 个窗口/病例，避免同一病例主导
    starts = rng.integers(0, max(1, len(co2) - N), size=3)
    for st in starts:
        seg = co2[st:st + N]
        if np.isnan(seg).mean() > 0.10:
            continue
        seg = fill_short_gaps(seg)
        _, seg = detect_outliers(seg)
        if np.isnan(seg).any():
            continue

        npk = len(find_breath_peaks(seg, SR))
        if npk == 0:
            key = 'p0'
        elif npk <= 5:
            key = 'p1_5'
        elif npk <= 15:
            key = 'p6_15'
        elif npk <= 30:
            key = 'p16_30'
        else:
            key = 'p31plus'

        if len(buckets[key]) < 4:
            buckets[key].append((os.path.basename(path)[:-8], st, seg.copy()))

    if all(len(v) >= 4 for v in buckets.values()):
        break

cases = []
for key, items in buckets.items():
    for cid, st, seg in items:
        cases.append((f'real_{cid}_s{st}_{key}', seg))

print(f'选出 {len(cases)} 个真实窗口')

with open(os.path.join(HERE, 'waves.txt'), 'w', encoding='ascii') as fw, \
     open(os.path.join(HERE, 'expected.txt'), 'w', encoding='ascii') as fe:
    fw.write(f'{len(cases)}\n')
    for name, w in cases:
        fw.write(f'{name} {len(w)}\n')
        fw.write(' '.join(f'{float(v):.9g}' for v in w) + '\n')
        feat = extract_features(w, SR)
        pk = find_breath_peaks(w, SR)
        fe.write(f'{name} peaks={len(pk)}\n')
        fe.write(' '.join(f'{float(v):.17g}' for v in feat) + '\n')

for key, items in buckets.items():
    print(f'  {key:8s}: {len(items)} 个')
