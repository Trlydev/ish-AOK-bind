#!/usr/bin/env python3
"""jitprof-gadget-pairs.py PROFILE... -- from the "arm64g" lines of an
ISH_JIT_PROFILE file (jit/jitprof.c), the gadget-level view of each block: the
gadgets that run most, and the adjacent pairs of single-gadget steps."""
import sys, collections
cols = []
minw = {}
data = []
for path in sys.argv[1:]:
    blocks = []
    for line in open(path):
        p = line.split()
        if len(p) < 4 or p[0] != 'arm64g': continue
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
print('gadget (words)          mean ' + ''.join('%8s' % c[0][-6:] for c in cols))
for k, v in names.most_common(60):
    print('%-20s %2d %6.2f ' % (k[:20], minw[k], 100 * v) + ''.join('%8.2f' % (100.0 * c[2][k] / c[1]) for c in cols))
print('\npair                                     mean ' + ''.join('%8s' % c[0][-6:] for c in cols))
for k, v in pn.most_common(90):
    print('%-20s %-20s %5.2f ' % (k[0][:20], k[1][:20], 100 * v) + ''.join('%8.2f' % (100.0 * c[3][k] / c[1]) for c in cols))
