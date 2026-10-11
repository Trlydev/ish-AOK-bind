#!/usr/bin/env python3
"""jitprof-gadget-pairs.py PROFILE... -- from the "arm64g" or "riscv64g" lines of an
ISH_JIT_PROFILE file (jit/jitprof.c), the gadget-level view of each block: the
gadgets that run most, and the adjacent pairs of single-gadget steps."""
import sys, collections, re
# --fuse PAIRS_H [--min SHARE]: instead of the report, print the FUSE2 lines
# for jit/guest-arm64/pairs.h -- every adjacent pair of gadgets that header
# gives a family to, whose mean share of the gadgets run is at least SHARE
# percent (default 0.01).
fuse = None; fuse_min = 0.01
argv = sys.argv[1:]
if argv and argv[0] == '--fuse':
    fuse = set(re.findall(r'^#define F_(\w+) ', open(argv[1]).read(), re.M)); argv = argv[2:]
if argv and argv[0] == '--min':
    fuse_min = float(argv[1]); argv = argv[2:]
cols = []
minw = {}
data = []
for path in argv:
    blocks = []
    for line in open(path):
        p = line.split()
        if len(p) < 4 or p[0] not in ('arm64g', 'riscv64g'): continue
        steps = []
        for t in p[3:]:
            name, w, n = t.rsplit('/', 2)
            steps.append((name, int(w), int(n)))
            if name not in minw or int(w) < minw[name]: minw[name] = int(w)
        blocks.append((int(p[1]), steps))
    data.append((path.split('/')[-1].split('.')[0], blocks))
for tag, blocks in data:
    total = 0; g = collections.Counter(); pairs = collections.Counter(); multi = collections.Counter()
    for n, steps in blocks:
        total += n * len(steps)
        for i, (name, w, k) in enumerate(steps):
            g[name] += n
            if w != minw[name]: multi[name] += n
            if i + 1 < len(steps) and w == minw[name]:
                pairs[(name, steps[i + 1][0])] += n
    cols.append((tag, total, g, pairs, multi))
names = collections.Counter(); pn = collections.Counter()
for tag, total, g, pairs, multi in cols:
    for k, v in g.items(): names[k] += v / total / len(cols)
    for k, v in pairs.items(): pn[k] += v / total / len(cols)
if fuse is not None:
    cut = 0.0
    for (a, b), v in pn.most_common():
        if a in fuse and b in fuse and 100 * v >= fuse_min:
            print('FUSE2(%s, %s)' % (a, b)); cut += v
    sys.stderr.write('%.1f%% of gadget dispatches (mean)\n' % (100 * cut))
    sys.exit(0)
print('gadget (words)          mean ' + ''.join('%8s' % c[0][-6:] for c in cols))
for k, v in names.most_common(60):
    print('%-20s %2d %6.2f ' % (k[:20], minw[k], 100 * v) + ''.join('%8.2f' % (100.0 * c[2][k] / c[1]) for c in cols))
print('\npair                                     mean ' + ''.join('%8s' % c[0][-6:] for c in cols))
for k, v in pn.most_common(90):
    print('%-20s %-20s %5.2f ' % (k[0][:20], k[1][:20], 100 * v) + ''.join('%8.2f' % (100.0 * c[3][k] / c[1]) for c in cols))
