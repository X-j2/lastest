# -*- coding: utf-8 -*-
"""上位机特征提取 vs 固件 C 实现 —— 一致性验证

背景：12 维特征存在三份实现
  1. 训练端 Python  features/extract_features.py   （基准，已验证）
  2. 固件 C         ndir-f407/AI/feature_extract.c （已验证 vs 基准）
  3. 上位机 Python  上位机代码/co2_monitor.py       （本脚本验证）

本脚本把上位机的特征函数从**真实源码**中提取出来执行（AST 提取，
不复制代码），与固件 C 的实现（经 gcc 编译）逐位比对。

两条链路：
  固件：2250 点 @25Hz → 重采样 9000 点 @100Hz → FE_Extract(fs=100)
  上位机：2250 点 @25Hz → extract_features(fs=25)      （不重采样）

【实测结论】两者在数学上**不等价**，差异来自 scipy 的 `prominence`
计算：固件在 4 倍插值后的 9000 点上算突出度，上位机在原始 2250 点上算。
插值会抬高局部极小值，使某些峰的突出度跨过 prominence=8 的门限而产生
微小的峰位差异。实测 60 个真实窗口：

    峰数不一致  0/60   （峰数总是相同）
    标签一致   60/60   （模型输出完全相同）

但 F6（峰间距标准差）最大相对误差达 31%，F12 达 17%，其余特征
F1/F2/F4/F7/F8 也有 1e-3 ~ 1e-2 量级的差异。

这意味着上位机**不能直接用自己的特征做最终判断**——它与端侧结果
存在可观测差异。改法是让上位机直接采用固件通过字节 11-14 发来的
结论（详见《上位机需配套修改》方案）；本地特征仅作交叉验证参考。
"""
import ast
import os
import subprocess
import sys
import textwrap

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HOST_SRC = os.path.join(os.path.dirname(ROOT), '上位机代码', 'co2_monitor.py')

HOST_METHODS = [
    '_find_breath_peaks', '_feature_stat', '_feature_periodicity',
    '_feature_width', '_feature_plateau', '_feature_peak_count_amp',
    '_feature_wave_rr', '_feature_rhythm_confidence', 'extract_features',
]

FEATURE_NAMES = ['F1 Mean', 'F2 Std', 'F3 PtP', 'F4 Skew',
                 'F5 IvMean', 'F6 IvSD', 'F7 Width', 'F8 Plateau',
                 'F9 PeakN', 'F10 Amp', 'F11 WaveRR', 'F12 Rhythm']


def extract_host_methods(path):
    """用 AST 从真实源码中取出方法体，拼成一个独立类后执行。"""
    with open(path, 'r', encoding='utf-8') as f:
        src = f.read()
    tree = ast.parse(src)
    lines = src.split('\n')

    found = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and node.name == 'CO2MonitorWindow':
            for item in node.body:
                if isinstance(item, ast.FunctionDef) and item.name in HOST_METHODS:
                    found[item.name] = (item.lineno, item.end_lineno)

    missing = [m for m in HOST_METHODS if m not in found]
    if missing:
        raise RuntimeError(f'源码中找不到方法: {missing}')

    body = ['class HostFeatures:']
    body.append('    AI_SAMPLE_RATE = 25.0')
    body.append('')
    for name in HOST_METHODS:
        lo, hi = found[name]
        chunk = '\n'.join(lines[lo - 1:hi])
        body.append(textwrap.indent(textwrap.dedent(chunk), '    '))
        body.append('')

    ns = {'np': np, 'find_peaks': None}
    from scipy.signal import find_peaks
    ns['find_peaks'] = find_peaks

    code = '\n'.join(body)
    exec(compile(code, '<host_extracted>', 'exec'), ns)
    return ns['HostFeatures']()


def firmware_features(wave_25hz):
    """调用 gcc 编译的固件 C 实现（重采样 + 特征）。"""
    with open(os.path.join(HERE, '_fw_wave.txt'), 'w') as f:
        f.write(' '.join(f'{float(v):.9g}' for v in wave_25hz))
    r = subprocess.run([os.path.join(HERE, '_fw_test.exe')],
                       cwd=HERE, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f'固件侧失败: {r.stderr}')
    vals = [float(v) for v in r.stdout.split()]
    return vals[:12], int(vals[12])


FW_SRC = os.path.join(HERE, 'fw_runner.c')


def build_firmware_runner():
    exe = os.path.join(HERE, '_fw_test.exe')
    cmd = ['gcc', '-O0', '-std=c99', '-I.', '-I../ndir-f407/AI',
           FW_SRC, '../ndir-f407/AI/feature_extract.c', '-lm', '-o', exe]
    r = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError('编译失败:\n' + r.stderr)
    return exe


def load_real_windows(n=60):
    """从缓存的 VitalDB 真实波形取 25Hz 数据（100Hz 每 4 点取 1 点）。"""
    import glob
    files = sorted(glob.glob(os.path.join(ROOT, 'cache', 'vitaldb', '*.npz')))
    rng = np.random.default_rng(4321)
    out = []
    for path in files:
        if len(out) >= n:
            break
        co2 = np.asarray(np.load(path)['co2'], dtype=float)
        if len(co2) < 9200:
            continue
        for st in rng.integers(0, 200, size=1):
            seg = co2[st:st + 9000]          # 100Hz × 90s
            if np.isnan(seg).mean() > 0.10:
                continue
            seg = np.nan_to_num(seg, nan=float(np.nanmean(seg)))
            out.append(seg[::4])             # → 2250 点 @25Hz
    return out


def main():
    if not os.path.exists(HOST_SRC):
        print(f'找不到上位机源码: {HOST_SRC}')
        return 1

    build_firmware_runner()
    host = extract_host_methods(HOST_SRC)
    print(f'上位机源码: {HOST_SRC}')
    print(f'已提取 {len(HOST_METHODS)} 个方法（AST 从真实源码提取）')
    print()

    windows = load_real_windows(60)
    print(f'测试窗口: {len(windows)} 个真实波形（VitalDB，降采样至 25Hz）')
    print()

    agree_labels = 0
    peak_mismatch = 0
    max_rel = [0.0] * 12
    worst = [''] * 12

    for idx, w25 in enumerate(windows):
        fw_feats, fw_np = firmware_features(w25)
        host_feats, host_np = host.extract_features(list(w25), 25.0)

        if fw_np != host_np:
            peak_mismatch += 1

        for i in range(12):
            a, b = fw_feats[i], host_feats[i]
            rel = abs(a - b) / max(abs(a), 1.0)
            if rel > max_rel[i]:
                max_rel[i] = rel
                worst[i] = f'窗口{idx}'

    print('各特征最大相对误差（固件 C vs 上位机 Python）:')
    for i in range(12):
        flag = '  ← 偏差较大' if max_rel[i] > 1e-4 else ''
        print(f'  {FEATURE_NAMES[i]:12s} {max_rel[i]:.3e}{flag}')
    print()
    print(f'峰数不一致的窗口数: {peak_mismatch}/{len(windows)}')
    print()

    identical = [i for i in range(12) if max_rel[i] <= 1e-4]
    print(f'一致到 1e-4 以内的特征: {len(identical)}/12')

    # 用模型验证标签是否一致
    import onnxruntime as ort
    sess = ort.InferenceSession(os.path.join(ROOT, 'etco2_model_v2.onnx'),
                                providers=['CPUExecutionProvider'])
    iname = sess.get_inputs()[0].name

    lab_fw, lab_host = [], []
    for w25 in windows:
        fw_f, _ = firmware_features(w25)
        ho_f, _ = host.extract_features(list(w25), 25.0)
        lab_fw.append(int(np.asarray(sess.run(
            None, {iname: np.array([fw_f], dtype=np.float32)})[0]).ravel()[0]))
        lab_host.append(int(np.asarray(sess.run(
            None, {iname: np.array([ho_f], dtype=np.float32)})[0]).ravel()[0]))

    agree = sum(1 for a, b in zip(lab_fw, lab_host) if a == b)
    print(f'预测标签一致: {agree}/{len(windows)}')
    if agree < len(windows):
        for i, (a, b) in enumerate(zip(lab_fw, lab_host)):
            if a != b:
                print(f'  窗口{i}: 固件={a} 上位机={b}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
