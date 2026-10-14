"""tab.py LOG... -- medians per build from abn.sh (or ab.sh) logs, each against the
first build in the log. 7z is MIPS, higher is better; the Python figures are ms.
"""
import re,collections,statistics,sys
def table(path, keyre):
    d=collections.defaultdict(lambda: collections.defaultdict(list)); order=[]
    for line in open(path):
        m=re.search(keyre, line)
        if not m or '7z=' not in line: continue
        arch=re.search(r' (\w+) \S+ 7z=', line)
        b=m.group(1) + (':' + arch.group(1) if arch else '')
        if b not in order: order.append(b)
        for k,v in re.findall(r'(7z|fib|method|dict|str)=(\d+)', line): d[b][k].append(int(v))
    keys=['7z','fib','method','dict','str']
    print('%-6s'%'build'+''.join('%14s'%k for k in keys))
    for b in order:
        row='%-6s'%b
        for k in keys:
            first=[o for o in order if o.split(':')[1:]==b.split(':')[1:]][0]
            m=statistics.median(d[b][k]); base=statistics.median(d[first][k])
            row+='%8.0f %+5.1f%%'%(m,(m-base)*100.0/base)
        print(row, ' n=%d'%len(d[b]['7z']), ' 7z:', d[b]['7z'])
for p in sys.argv[1:]:
    print(p.split('/')[-1]); table(p, r'r\d-b(\w+) ')
