# -*- coding: utf-8 -*-
"""比对 Python 参考特征与 C 实现输出。"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))

FEATURE_NAMES = ['F1 Mean', 'F2 Std', 'F3 PtP', 'F4 Skew',
                 'F5 IvMean', 'F6 IvSD', 'F7 Width', 'F8 Plateau',
                 'F9 PeakN', 'F10 Amp', 'F11 WaveRR', 'F12 Rhythm']


def load(path):
    out = {}
    with open(path, 'r', encoding='ascii') as f:
        while True:
            head = f.readline()
            if not head:
                break
            head = head.strip()
            if not head:
                continue
            parts = head.split()
            name = parts[0]
            peaks = int(parts[1].split('=')[1])
            vals = [float(v) for v in f.readline().split()]
            out[name] = (peaks, vals)
    return out


exp = load(os.path.join(HERE, 'expected.txt'))
got = load(os.path.join(HERE, 'c_out.txt'))

assert set(exp) == set(got), f'用例集合不一致:\n exp={sorted(exp)}\n got={sorted(got)}'

n_pass = 0
n_fail = 0
worst = (0.0, '', '')

for name in sorted(exp):
    ep, ev = exp[name]
    gp, gv = got[name]

    if ep != gp:
        print(f'[FAIL] {name}: 峰数 Python={ep} C={gp}')
        n_fail += 1
        continue

    bad = []
    for i, (a, b) in enumerate(zip(ev, gv)):
        denom = max(abs(a), 1.0)
        rel = abs(a - b) / denom
        if rel > worst[0]:
            worst = (rel, name, FEATURE_NAMES[i])
        if rel > 1e-4:
            bad.append(f'{FEATURE_NAMES[i]}(py={a:.10g} c={b:.10g} rel={rel:.2e})')

    if bad:
        print(f'[FAIL] {name}')
        for b in bad:
            print(f'        {b}')
        n_fail += 1
    else:
        n_pass += 1
        print(f'[ ok ] {name:24s} peaks={ep}')

print()
print(f'通过 {n_pass} / {n_pass + n_fail}')
print(f'最大相对误差 {worst[0]:.3e}  @ {worst[1]} / {worst[2]}')
