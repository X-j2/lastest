# -*- coding: utf-8 -*-
"""端到端推理链路验证（三方交叉验证）

A. 从 ONNX 读权威 TreeEnsembleClassifier 参数
B. 与固件里的 C 权重数组逐元素比对
   -> 验证「部署到板子上的权重」与「训练出的 ONNX 模型」完全一致
C. 只用 C 侧参数独立实现一遍森林遍历，与 onnxruntime 的预测标签比对
   -> 验证遍历语义（即 X-CUBE-AI 运行库会算出同样结果）
"""
import os
import sys
import numpy as np
import onnx
import onnxruntime as ort

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def read_c_params(path):
    """读 gen_rf_ref.c 从 C 权重数组 dump 出来的参数。"""
    p = {}
    nodes, weights, classnodeids = [], [], []
    mode = None
    with open(path, 'r', encoding='ascii') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line == 'nodes_begin':
                mode = 'nodes'; continue
            if line == 'nodes_end':
                mode = None; continue
            if line == 'weights_begin':
                mode = 'weights'; continue
            if line == 'weights_end':
                mode = None; continue
            if line == 'classnodeids_begin':
                mode = 'cni'; continue
            if line == 'classnodeids_end':
                mode = None; continue
            if mode == 'nodes':
                a, b, c, d = line.split()
                nodes.append((int(a), int(b), float(c), int(d)))
            elif mode == 'weights':
                weights.append(float(line))
            elif mode == 'cni':
                classnodeids.append(int(line))
            else:
                k, *v = line.split()
                p[k] = v
    return (p,
            np.array(nodes, dtype=np.float64),
            np.array(weights, dtype=np.float64),
            np.array(classnodeids, dtype=np.int64))


def onnx_attrs(model_path):
    m = onnx.load(model_path)
    node = [n for n in m.graph.node if n.op_type == 'TreeEnsembleClassifier'][0]
    out = {}
    for a in node.attribute:
        if a.type == onnx.AttributeProto.INTS:
            out[a.name] = np.array(list(a.ints), dtype=np.int64)
        elif a.type == onnx.AttributeProto.FLOATS:
            out[a.name] = np.array(list(a.floats), dtype=np.float64)
    return out


def walk_c(c_nodes, c_weights, c_classnodeids, c_meta, feats, n_classes=4):
    """只用 C 侧参数遍历。c_nodes 每行: featureid, truenodeid, value, falsenodeid"""
    n_trees = int(c_meta['n_trees'][0])
    nb_node = [int(x) for x in c_meta['nb_node']]
    nb_class = [int(x) for x in c_meta['nb_class']]
    classlabels = [int(x) for x in c_meta['classlabels']]

    # class_weights 按树分块；块内 entry 的 class_nodeids 给出所属节点号
    scores = np.zeros(n_classes, dtype=np.float64)
    w_off = 0
    n_off = 0

    for t in range(n_trees):
        nn = nb_node[t]
        nc = nb_class[t]

        # 本树的 (node_id -> class weight) 映射
        leaf_w = {}
        for k in range(nc):
            nid = int(c_classnodeids[w_off + k])
            leaf_w.setdefault(nid, [0.0] * n_classes)
            leaf_w[nid][classlabels[k % n_classes]] += c_weights[w_off + k]
        w_off += nc

        # 本树节点在全局数组中的行范围
        base = n_off
        # 局部 node id 即行内偏移（nodeids 从 0 连续编号）
        j = base
        guard = 0
        while True:
            tn = int(c_nodes[j][1])
            fn = int(c_nodes[j][3])
            if tn == fn:
                local_node_id = j - base
                for cls, val in enumerate(leaf_w.get(local_node_id, [0.0] * n_classes)):
                    scores[cls] += val
                break
            if feats[int(c_nodes[j][0])] <= c_nodes[j][2]:
                j = base + tn
            else:
                j = base + fn
            guard += 1
            if guard > nn + 4:
                raise RuntimeError(f'不收敛 tree={t}')
        n_off += nn

    return scores


def load_feats(path):
    o = {}
    with open(path) as f:
        while True:
            h = f.readline()
            if not h:
                break
            h = h.strip()
            if not h:
                continue
            t = h.split()
            o[t[0]] = [float(v) for v in f.readline().split()]
    return o


def main():
    attrs = onnx_attrs(os.path.join(ROOT, 'etco2_model_v2.onnx'))
    meta, c_nodes, c_weights, c_cni = read_c_params(os.path.join(HERE, 'rf_params.txt'))

    print('=== A. 固件中的 C 权重数组 vs ONNX 模型 ===')
    checks = [
        ('nodes_featureids', c_nodes[:, 0], attrs['nodes_featureids']),
        ('nodes_truenodeids', c_nodes[:, 1], attrs['nodes_truenodeids']),
        ('nodes_values', c_nodes[:, 2], attrs['nodes_values']),
        ('nodes_falsenodeids', c_nodes[:, 3], attrs['nodes_falsenodeids']),
        ('class_weights', c_weights, attrs['class_weights']),
        ('class_nodeids', c_cni, attrs['class_nodeids']),
    ]
    all_ok = True
    for name, mine, ref in checks:
        if len(mine) != len(ref):
            print(f'  {name:20s} 长度不一致 mine={len(mine)} onnx={len(ref)}')
            all_ok = False
            continue
        same = np.array_equal(mine.astype(np.float32), np.asarray(ref).astype(np.float32))
        print(f'  {name:20s} {"一致" if same else "不一致"}  (n={len(ref)})')
        all_ok &= same
    print(f'  -> {"固件权重与 ONNX 逐元素一致" if all_ok else "存在差异"}')
    print()

    print('=== B. 只用 C 参数遍历 vs onnxruntime ===')
    feats = load_feats(os.path.join(HERE, 'expected.txt'))
    names = sorted(feats)
    X = np.array([feats[k] for k in names], dtype=np.float32)

    sess = ort.InferenceSession(os.path.join(ROOT, 'etco2_model_v2.onnx'),
                               providers=['CPUExecutionProvider'])
    onnx_lab = np.asarray(sess.run(None, {sess.get_inputs()[0].name: X})[0]).ravel()

    my_lab = np.array([
        int(np.argmax(walk_c(c_nodes, c_weights, c_cni, meta, row.astype(np.float64))))
        for row in X
    ])

    agree = int((onnx_lab == my_lab).sum())
    print(f'  标签一致: {agree}/{len(names)}')
    if agree != len(names):
        bad = [names[i] for i in range(len(names)) if onnx_lab[i] != my_lab[i]]
        print(f'  不一致: {bad[:10]}')
    print()
    print('=== 结论 ===')
    if all_ok and agree == len(names):
        print('  固件权重与 ONNX 一致，遍历语义一致 -> 端侧推理链路正确')
        return 0
    print('  存在不一致，需排查')
    return 1


if __name__ == '__main__':
    sys.exit(main())
