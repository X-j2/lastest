# -*- coding: utf-8 -*-
"""大规模真实数据测试：120 个窗口，量化浮点平局导致的特征偏差。"""
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
rng = np.random.default_rng(2024)

cases = []
for path in files:
    if len(cases) >= 120:
        break
    data = np.load(path)
    co2 = np.asarray(data['co2'], dtype=float)
    if len(co2) < N:
        continue
    for st in rng.integers(0, max(1, len(co2) - N), size=2):
        if len(cases) >= 120:
            break
        seg = co2[st:st + N]
        if np.isnan(seg).mean() > 0.10:
            continue
        seg = fill_short_gaps(seg)
        _, seg = detect_outliers(seg)
        if np.isnan(seg).any() or not np.isfinite(seg).all():
            continue
        cases.append((f'w{len(cases):03d}', seg.copy()))

print(f'生成 {len(cases)} 个真实窗口')

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

print('完成')
