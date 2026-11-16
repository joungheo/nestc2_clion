#!/usr/bin/env python3
"""Compute only the rows (tokens) an ONNX model's outputs use. usage: prune_rows_onnx.py in.onnx out.onnx [in.yaml out.yaml [extra.yaml]]

#### JO added (2026-11-16, docs/deit_optimization.md 6.7): DeiT reads rows 0 and 1 (the class and distillation tokens) of its
#### last layer norm (two Gathers on axis 1); every op after the last attention mixing is row-wise, and so is the query side
#### of that attention. A backward liveness pass finds, for each tensor, the index range [lo, hi) along one axis that its
#### consumers read (or 'full'); the rewrite then computes each tensor with a partial range only on those rows: a Slice where a
#### full tensor feeds a partial one (the residual stream, the layer-norm output before the query MatMul), Reshape shapes and
#### Gather indices adjusted. Exact (the same arithmetic on fewer rows).
#### Rules (output axis a, range [lo, hi) -> inputs): Gather(axis a, constant indices) asks its data for those indices whatever its
#### output needs; elementwise ops and ReduceMean / Softmax over other axes pass the range to inputs of the same size on that axis
#### (broadcast inputs of size 1 and constants: whole); MatMul passes a row range to the left operand only (the right one whole);
#### Transpose and Reshape map the axis (Reshape when the dims before it keep their product); anything else needs its inputs whole.
#### With the yaml arguments the quantization profile is copied with an entry for each new Slice (that of its input under the
#### Glow name of the input's producer), so every scale stays the same and the int8 values of the kept rows are those of before.
"""
import sys

import numpy as np
import onnx
from onnx import helper, numpy_helper, shape_inference

FULL = 'full'
ELEMWISE = {'Add', 'Sub', 'Mul', 'Div', 'Pow', 'Sqrt', 'Erf', 'Relu', 'Tanh', 'Sigmoid', 'Exp', 'Neg', 'Abs', 'Clip', 'Cast',
            'Identity'}


def merge(a, b):
    if a is None:
        return b
    if b is None:
        return a
    if a == FULL or b == FULL or a[0] != b[0]:
        return FULL
    return (a[0], min(a[1], b[1]), max(a[2], b[2]))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    m = onnx.load(src)
    g = m.graph
    mi = shape_inference.infer_shapes(m)
    shape = {}
    for vi in list(mi.graph.value_info) + list(mi.graph.input) + list(mi.graph.output):
        shape[vi.name] = [d.dim_value for d in vi.type.tensor_type.shape.dim]
    inits = {t.name: t for t in g.initializer}
    for t in g.initializer:
        shape[t.name] = list(t.dims)
    const = {}   # Constant nodes too
    for n in g.node:
        if n.op_type == 'Constant':
            const[n.output[0]] = numpy_helper.to_array(n.attribute[0].t)
    def cval(name):
        if name in inits:
            return numpy_helper.to_array(inits[name])
        return const.get(name)
    def is_const(name):
        return name in inits or name in const

    live = {o.name: FULL for o in g.output}
    need = {}   # (node index, input index) -> requirement of that node on that input

    def full_dim(t, r):
        return r != FULL and shape.get(t) and r[1] == 0 and r[2] == shape[t][r[0]]

    for ni in reversed(range(len(g.node))):
        n = g.node[ni]
        if n.op_type == 'Constant':
            continue
        r = live.get(n.output[0])
        if r is not None and full_dim(n.output[0], r):
            r = FULL
        reqs = [FULL] * len(n.input)
        op = n.op_type
        attrs = {a.name: helper.get_attribute_value(a) for a in n.attribute}
        if op == 'Gather' and is_const(n.input[1]):
            idx = np.array(cval(n.input[1])).reshape(-1)
            ax = attrs.get('axis', 0) % len(shape[n.input[0]])
            reqs[0] = (ax, int(idx.min()), int(idx.max()) + 1)
        elif r is None:
            continue   # dead
        elif r == FULL:
            pass
        else:
            a, lo, hi = r
            ro = len(shape[n.output[0]])
            def same_axis(t, ai):
                st = shape.get(t)
                if st is None or is_const(t):
                    return FULL
                k = ai - (ro - len(st))
                if k < 0 or st[k] == 1:
                    return FULL
                return (k, lo, hi)
            if op in ELEMWISE:
                reqs = [same_axis(t, a) if t else FULL for t in n.input]
            elif op in ('ReduceMean', 'ReduceSum', 'Softmax', 'LogSoftmax'):
                axes = attrs.get('axes', [attrs.get('axis', -1)])
                axes = [x % ro for x in axes]
                if a not in axes:
                    reqs[0] = same_axis(n.input[0], a)
            elif op == 'MatMul':
                if a == ro - 2:
                    reqs[0] = same_axis(n.input[0], a)
                elif a < ro - 2:
                    reqs = [same_axis(t, a) for t in n.input]
            elif op == 'Transpose':
                reqs[0] = (attrs['perm'][a], lo, hi)
            elif op == 'Reshape':
                si, so = shape[n.input[0]], shape[n.output[0]]
                pre = int(np.prod(so[:a]))
                for b in range(len(si)):
                    if int(np.prod(si[:b])) == pre and si[b] == so[a]:
                        reqs[0] = (b, lo, hi)
                        break
            # else: whole inputs
        for k, t in enumerate(n.input):
            if not t or is_const(t):
                continue
            need[(ni, k)] = reqs[k]
            live[t] = merge(live.get(t), reqs[k])
    for t in list(live):
        if live[t] != FULL and live[t] is not None and full_dim(t, live[t]):
            live[t] = FULL

    # rewrite
    new_nodes, new_inits, slices = [], [], []   # slices: (slice output, its input)
    cnt = [0]
    def init(name, arr):
        t = numpy_helper.from_array(np.array(arr), name)
        new_inits.append(t)
        return name
    for ni, n in enumerate(g.node):
        if n.op_type == 'Constant':
            new_nodes.append(n)
            continue
        out_r = live.get(n.output[0])
        partial_out = out_r not in (None, FULL)
        for k, t in enumerate(list(n.input)):
            q = need.get((ni, k))
            if q in (None, FULL) or (n.op_type == 'Gather' and k == 0 and is_const(n.input[1])):
                continue   # a Gather reads its rows by index (remapped below)
            have = live.get(t)
            if have == q:
                continue
            # the input is computed whole (or on a larger range): slice what this node reads
            base = 0 if have in (None, FULL) else have[1]
            cnt[0] += 1
            nm = n.name + '/prune_slice%d' % cnt[0]
            so = nm + '_output_0'
            new_nodes.append(helper.make_node(
                'Slice', [t, init(nm + '_starts', [q[1] - base]), init(nm + '_ends', [q[2] - base]),
                          init(nm + '_axes', [q[0]])], [so], name=nm))
            slices.append((so, t, nm))
            n.input[k] = so
        if n.op_type == 'Reshape' and partial_out:
            so = list(shape[n.output[0]])
            so[out_r[0]] = out_r[2] - out_r[1]
            n.input[1] = init(n.name + '/prune_shape', np.array(so, dtype=np.int64))
        if n.op_type == 'Gather' and is_const(n.input[1]):
            have = live.get(n.input[0])
            if have not in (None, FULL):
                idx = np.array(cval(n.input[1]))
                n.input[1] = init(n.name + '/prune_indices', (idx - have[1]).astype(idx.dtype))
        new_nodes.append(n)
    del g.node[:]
    g.node.extend(new_nodes)
    g.initializer.extend(new_inits)
    del g.value_info[:]
    onnx.checker.check_model(m)
    m = shape_inference.infer_shapes(m)
    onnx.save(m, dst)
    part = sorted((t, r) for t, r in live.items() if r not in (None, FULL))
    print('prune_rows_onnx: %d tensors on part of their rows, %d slices' % (len(part), len(slices)))
    for so, t, nm in slices:
        print('  slice %s <- %s' % (nm, t))

    if len(sys.argv) > 4:
        # profile: an entry per Slice, copied from the entry of its input (Glow names the node output <node name>:0, '/' and
        # '.' as '_'; the profile keys carry suffixes such as __1 from Glow's copies: every suffix is copied)
        ysrc, ydst = sys.argv[3], sys.argv[4]
        prod = {o: n.name for n in g.node for o in n.output}
        text = open(ysrc).read()
        def glow(name):
            return ''.join(c if c.isalnum() else '_' for c in name)
        blocks = text.split('\n- NodeOutputName:')
        add = []
        for so, t, nm in slices:
            src_name = glow(prod.get(t, t))
            found = [b for b in blocks[1:] if b.strip().startswith("'" + src_name + ':') or
                     b.strip().startswith("'" + src_name + '__')]
            if not found:
                print('  no profile entry for %s (input of %s)' % (src_name, nm))
                continue
            b = found[0]
            key = b.strip().split("'")[1]
            for suf in ('', '__1', '__2'):   # the names of Glow's copies of the node
                add.append(b.replace("'" + key + "'", "'" + glow(nm) + suf + ":0'", 1))
            print('  profile %s[__1|__2]:0 <- %s' % (glow(nm), key))
        # extra.yaml (a profile of the pruned model): the entries of names the whole model's profile lacks (nodes that
        # exist only at the smaller shape, e.g. a broadcast constant Glow folds at [1, 2, 1] but not at [1, 198, 1])
        if len(sys.argv) > 5:
            have = set(b.strip().split("'")[1] for b in blocks[1:])
            have |= set(b.strip().split("'")[1] for b in add)
            extra = open(sys.argv[5]).read().rstrip('\n')
            if extra.endswith('\n...'):
                extra = extra[:-4]
            nx = 0
            for b in extra.split('\n- NodeOutputName:')[1:]:
                k = b.strip().split("'")[1]
                if k not in have:
                    add.append(b)
                    nx += 1
            print('  %d entries from %s' % (nx, sys.argv[5]))
        # inside the last YAML document (before its end marker '...')
        body = text.rstrip('\n')
        tail = ''
        if body.endswith('\n...'):
            body, tail = body[:-4], '\n...'
        open(ydst, 'w').write(body + ''.join('\n- NodeOutputName:' + b.rstrip('\n') for b in add) + tail + '\n')


if __name__ == '__main__':
    main()
