#!/usr/bin/env python3
"""Find gadgets that read a vector register a C call may have changed.

save_c/restore_c keep x0-x18 and lr. The AAPCS64 lets a callee change all of
v0-v7 and v16-v31 and the high halves of v8-v15, so a gadget that computes a
value into a vector register, calls something that can reach C (a TLB slow
path, a helper), and reads the register afterwards is reading whatever the
callee left there. a4e9b84fc was that bug in the arm64 MOPS SET gadget, and it
showed only on an A10X host.

This reads the assembled gadget objects, not the macro sources: it
disassembles each jit_*.S.o with objdump, builds the control flow, and runs a
forward "may be changed by a C call" analysis over v0-v31. A call to another
assembly routine is followed through a summary of that routine, so the hazard
is found whether the C call sits in the gadget or three helpers down.

usage: gadget-vreg-audit.py [-v] [--entry-reads] OBJECT.o... | BUILD_DIR
       gadget-vreg-audit.py --self-test
exit status 1 when anything is reported, or when the self-test fails.

-v lists each assembly routine's summary. --entry-reads lists gadgets that
may read a vector register before writing it: vector state handed from one
gadget to the next would show there, and so does any pair of branches on the
same flag (load if set ... use if set), so that list is for reading, not a
pass/fail.
"""

import glob
import os
import re
import subprocess
import sys
from collections import defaultdict

ALL = 0xFFFFFFFF
# What a C callee may change: every high half, and every low half but
# v8-v15's. The two halves are tracked apart, which is also what lets
# `mov.d v4[0], x9` then `mov.d v4[1], x9` count as writing all of v4.
C_LO = 0xFFFF00FF
C_HI = ALL

# The first operand is a vector register and is not written.
NO_DEST = {
    'str', 'stur', 'stp', 'stnp', 'st1', 'st2', 'st3', 'st4',
    'fcmp', 'fcmpe', 'fccmp', 'fccmpe',
}
TWO_DESTS = {'ldp', 'ldnp'}
LIST_DEST = {'ld1', 'ld2', 'ld3', 'ld4', 'ld1r', 'ld2r', 'ld3r', 'ld4r'}
# The destination is also a source.
DESTRUCTIVE = {
    'mla', 'mls', 'fmla', 'fmls', 'fmlal', 'fmlal2', 'fmlsl', 'fmlsl2',
    'smlal', 'smlal2', 'smlsl', 'smlsl2', 'umlal', 'umlal2', 'umlsl', 'umlsl2',
    'sqdmlal', 'sqdmlal2', 'sqdmlsl', 'sqdmlsl2', 'sqrdmlah', 'sqrdmlsh',
    'saba', 'uaba', 'sabal', 'sabal2', 'uabal', 'uabal2', 'sadalp', 'uadalp',
    'ssra', 'usra', 'srsra', 'ursra', 'suqadd', 'usqadd',
    'sli', 'sri', 'ins', 'bsl', 'bit', 'bif', 'tbx',
    'aese', 'aesd', 'sha1c', 'sha1p', 'sha1m', 'sha1su0', 'sha1su1',
    'sha256h', 'sha256h2', 'sha256su0', 'sha256su1',
    'sha512h', 'sha512h2', 'sha512su0', 'sha512su1',
    'sm3tt1a', 'sm3tt1b', 'sm3tt2a', 'sm3tt2b', 'sm3partw1', 'sm3partw2',
    'sm4e', 'sdot', 'udot', 'usdot', 'sudot', 'bfdot', 'bfmmla', 'smmla',
    'ummla', 'usmmla', 'bfmlalb', 'bfmlalt', 'fcmla',
}
# Write the high half and keep the low. (Not "ends in n2": trn2 does too.)
NARROW_HIGH = {
    'xtn2', 'sqxtn2', 'uqxtn2', 'sqxtun2', 'shrn2', 'rshrn2', 'sqshrn2',
    'uqshrn2', 'sqrshrn2', 'uqrshrn2', 'sqshrun2', 'sqrshrun2', 'addhn2',
    'raddhn2', 'subhn2', 'rsubhn2', 'fcvtn2', 'fcvtxn2', 'bfcvtn2',
}
COND_BRANCH = {'cbz', 'cbnz', 'tbz', 'tbnz'}
STOP = {'udf', 'brk', '.long', '.byte', '.short', '.quad', '<unknown>'}

VREG = re.compile(r'(?<![\w.])([vqdshb])(\d{1,2})(?![\w])(\[(\d+)\])?')
SYM_LINE = re.compile(r'^([0-9a-f]+) <(.+)>:$')
INSN_LINE = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
RELOC_LINE = re.compile(r'^\s+([0-9a-f]+):\s+ARM64_RELOC_BRANCH26\s+(\S+)')
LANE_BYTES = {'b': 1, 'h': 2, 's': 4, 'd': 8}


def split_operands(text):
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch in '[{':
            depth += 1
        elif ch in ']}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def halves(text, arr):
    """The vector registers `text` names, as (low-half mask, high-half mask).
    A scalar name (b/h/s/d) is the low half. A lane is the half it lies in
    when the arrangement says how wide it is; anything else is the whole
    register."""
    lo = hi = 0
    for kind, num, lane, idx in VREG.findall(text):
        n = int(num)
        if n > 31:
            continue
        bit = 1 << n
        if kind in 'bhsd':
            lo |= bit
        elif lane and arr in LANE_BYTES:
            if int(idx) * LANE_BYTES[arr] < 8:
                lo |= bit
            else:
                hi |= bit
        else:
            lo |= bit
            hi |= bit
    return lo, hi


def classify(mnem, optext):
    """(use_lo, use_hi, def_lo, def_hi) as masks over v0-v31.

    objdump prints Apple syntax: the arrangement rides on the mnemonic
    (`add.16b v4, v4, v5`, `mov.d v4[1], x9`)."""
    mnem, _, arr = mnem.partition('.')
    ops = split_operands(optext)
    if not ops:
        return 0, 0, 0, 0
    lo, hi = halves(optext, arr)
    if not lo | hi:
        return 0, 0, 0, 0
    flo, fhi = halves(ops[0], arr)
    if not flo | fhi or mnem in NO_DEST:
        return lo, hi, 0, 0
    if mnem in TWO_DESTS:
        slo, shi = halves(ops[1], arr)
        ulo, uhi = halves(','.join(ops[2:]), arr)
        both = flo | fhi | slo | shi
        return ulo, uhi, both, both
    ulo, uhi = halves(','.join(ops[1:]), arr)
    whole = flo | fhi
    lane = bool(re.search(r'\}\s*\[', ops[0])) if mnem in LIST_DEST \
        else '[' in ops[0]
    if lane:
        # mov.d v4[1], x9 / ld1.d { v0 }[1], [x0]: one lane written, the
        # rest kept. Keeping is not reading -- whoever reads the register
        # next is the reader -- and a 64-bit lane is a whole half written.
        if arr == 'd':
            return ulo, uhi, flo & ~fhi, fhi & ~flo
        return ulo, uhi, 0, 0
    if mnem in NARROW_HIGH:
        # xtn2 and friends write the high half and keep the low
        return ulo, uhi, 0, whole
    if mnem in DESTRUCTIVE \
            or (mnem in ('orr', 'bic') and len(ops) == 2 and '#' in ops[1]):
        return ulo | whole, uhi | whole, 0, 0
    if mnem == 'eor' and len(ops) == 3 and ops[0] == ops[1] == ops[2]:
        return 0, 0, whole, whole            # the zeroing idiom
    return ulo, uhi, whole, whole


class Insn:
    __slots__ = ('addr', 'mnem', 'ops', 'use_lo', 'use_hi', 'def_lo',
                 'def_hi', 'kind', 'target')


class Obj:
    def __init__(self, path, index):
        self.path = path
        self.name = os.path.basename(path).replace('.S.o', '.S')
        self.index = index
        self.insns = {}
        self.syms = {}          # name -> addr
        self.sym_at = {}        # addr -> name (first seen)
        self.externs = set()    # names nm calls external (T)
        self.relocs = {}

    def load(self):
        nm = subprocess.run(['nm', self.path], capture_output=True, text=True,
                            check=True).stdout
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] == 'T':
                self.externs.add(parts[2])
        text = subprocess.run(
            ['objdump', '-d', '-r', '--no-show-raw-insn', self.path],
            capture_output=True, text=True, check=True).stdout
        raw = []
        for line in text.splitlines():
            m = SYM_LINE.match(line)
            if m:
                addr = int(m.group(1), 16)
                self.syms.setdefault(m.group(2), addr)
                self.sym_at.setdefault(addr, m.group(2))
                continue
            m = RELOC_LINE.match(line)
            if m:
                self.relocs[int(m.group(1), 16)] = m.group(2)
                continue
            m = INSN_LINE.match(line)
            if m:
                raw.append((int(m.group(1), 16), m.group(2), m.group(3)))
        for addr, mnem, ops in raw:
            ops = ops.split(';')[0]
            ops = re.sub(r'<[^>]*>', '', ops).strip()
            i = Insn()
            i.addr, i.mnem, i.ops = addr, mnem, ops
            i.kind, i.target = 'plain', None
            i.use_lo = i.use_hi = i.def_lo = i.def_hi = 0
            if mnem in STOP:
                i.kind = 'stop'
            elif mnem in ('ret', 'retaa', 'retab'):
                i.kind = 'ret'
            elif mnem in ('br', 'braa', 'brab', 'braaz', 'brabz'):
                i.kind = 'exit'
            elif mnem in ('blr', 'blraa', 'blrab', 'blraaz', 'blrabz'):
                i.kind = 'ccall'
            elif mnem in ('b', 'bl') or mnem.startswith('b.') \
                    or mnem in COND_BRANCH:
                i.kind = {'b': 'jump', 'bl': 'call'}.get(mnem, 'cond')
                i.target = self.relocs.get(addr)
                if i.target is None:
                    last = split_operands(ops)[-1]
                    i.target = int(last, 16)
            else:
                i.use_lo, i.use_hi, i.def_lo, i.def_hi = classify(mnem, ops)
            self.insns[addr] = i
        self.find_tables()

    def find_tables(self):
        """A `br` whose register was not loaded from the gadget stream is a
        jump table inside one gadget (per-size or per-register bodies). Its
        targets are not decoded: the state is carried to every instruction
        of the enclosing global symbol, which is a superset of them."""
        starts = sorted(self.syms[n] for n in self.externs if n in self.syms)
        self.tables = 0
        for addr, i in self.insns.items():
            if i.kind != 'exit':
                continue
            reg = i.ops.strip()
            from_stream = False
            for back in range(1, 9):
                j = self.insns.get(addr - 4 * back)
                if j is None:
                    break
                ops = split_operands(j.ops)
                if j.mnem in ('ldr', 'ldur', 'ldp') and reg in ops[:2] \
                        and 'x28' in j.ops:
                    from_stream = True
                    break
            if from_stream:
                continue
            lo = max([a for a in starts if a <= addr], default=0)
            hi = min([a for a in starts if a > addr],
                     default=max(self.insns) + 4)
            i.kind = 'table'
            i.target = (lo, hi)
            self.tables += 1

    def where(self, addr):
        best = None
        for a, name in self.sym_at.items():
            if a <= addr and (best is None or a > best[0]) \
                    and not name.startswith('ltmp'):
                best = (a, name)
        if best is None:
            return '%s+0x%x' % (self.name, addr)
        return '%s+0x%x' % (best[1], addr - best[0]) if addr != best[0] \
            else best[1]


class Audit:
    def __init__(self, objs):
        self.objs = objs
        self.globals = {}
        for o in objs:
            for name in o.externs:
                if name in o.syms:
                    self.globals[name] = (o.index, o.syms[name])
        self.summary = {}       # (obj, addr) -> (gen_lo, gen_hi, ue, kept_lo, kept_hi)
        self.returns = {}
        self.reports = {}       # (obj, addr, reg) -> text
        self.entry_reads = defaultdict(int)   # (obj, addr) -> mask

    def resolve(self, o, target):
        """A branch target as (obj index, addr), or None for C."""
        if isinstance(target, int):
            return (o.index, target) if target in o.insns else None
        if target in o.syms:
            return (o.index, o.syms[target])
        return self.globals.get(target)

    def run(self, seeds, as_function):
        """Forward analysis over (lo, hi, klo, khi): the halves a C call may
        have changed, and the halves that may still hold what they held on
        entry. Returns the reads of changed halves it met; for a function
        run, also the function's summary."""
        state = {}
        work = []
        for s in seeds:
            state[s] = (0, 0, ALL, ALL)
            work.append(s)
        gen_lo = gen_hi = ue = ret_klo = ret_khi = 0
        returned = False
        found = []

        def push(key, lo, hi, klo, khi):
            old = state.get(key)
            if old is None:
                state[key] = (lo, hi, klo, khi)
                work.append(key)
            else:
                new = (old[0] | lo, old[1] | hi, old[2] | klo, old[3] | khi)
                if new != old:
                    state[key] = new
                    work.append(key)

        while work:
            key = work.pop()
            oi, addr = key
            o = self.objs[oi]
            i = o.insns.get(addr)
            if i is None:
                continue
            lo, hi, klo, khi = state[key]
            nxt = (oi, addr + 4)
            k = i.kind
            if k == 'plain':
                if i.use_lo | i.use_hi:
                    bad = (lo & i.use_lo) | (hi & i.use_hi)
                    if bad:
                        found.append((key, bad))
                    ue |= ((klo & i.use_lo) | (khi & i.use_hi)) & ~bad
                if i.def_lo | i.def_hi:
                    lo &= ~i.def_lo
                    hi &= ~i.def_hi
                    klo &= ~i.def_lo
                    khi &= ~i.def_hi
                push(nxt, lo, hi, klo, khi)
            elif k == 'stop' or k == 'exit':
                pass
            elif k == 'table':
                for t in range(i.target[0], i.target[1], 4):
                    push((oi, t), lo, hi, klo, khi)
            elif k == 'ret':
                returned = True
                gen_lo |= lo
                gen_hi |= hi
                ret_klo |= klo
                ret_khi |= khi
            elif k == 'ccall':
                push(nxt, lo | C_LO, hi | C_HI, klo, khi)
            elif k == 'cond':
                t = self.resolve(o, i.target)
                if t:
                    push(t, lo, hi, klo, khi)
                push(nxt, lo, hi, klo, khi)
            elif k == 'jump':
                t = self.resolve(o, i.target)
                if t:
                    push(t, lo, hi, klo, khi)
                else:               # tail call into C
                    returned = True
                    gen_lo |= lo | C_LO
                    gen_hi |= hi | C_HI
                    ret_klo |= klo
                    ret_khi |= khi
            elif k == 'call':
                t = self.resolve(o, i.target)
                if t is None:
                    push(nxt, lo | C_LO, hi | C_HI, klo, khi)
                elif self.returns.get(t):
                    glo, ghi, u, plo, phi = self.summary[t]
                    bad = (lo | hi) & u
                    if bad:
                        found.append((key, bad))
                    ue |= (klo | khi) & u & ~bad
                    push(nxt, glo | (lo & plo), ghi | (hi & phi),
                         klo & plo, khi & phi)
        if as_function:
            return (gen_lo, gen_hi, ue, ret_klo, ret_khi), returned, found
        return found

    def analyse(self):
        funcs = set()
        for o in self.objs:
            for i in o.insns.values():
                if i.kind == 'call':
                    t = self.resolve(o, i.target)
                    if t:
                        funcs.add(t)
        self.funcs = funcs
        self.returns = {}
        changed = True
        rounds = 0
        while changed:
            changed = False
            rounds += 1
            for f in sorted(funcs):
                new, returned, _ = self.run([f], True)
                if self.summary.get(f) != new or self.returns.get(f) != returned:
                    self.summary[f] = new
                    self.returns[f] = returned
                    changed = True
        seeds = []
        for o in self.objs:
            for addr in o.sym_at:
                if addr in o.insns:
                    seeds.append((o.index, addr))
        seeds += list(funcs)
        return self.run(seeds, False), rounds

    def origins(self, key, reg):
        """Call sites whose change to `reg` reaches `key`."""
        if not hasattr(self, 'preds'):
            preds = defaultdict(list)
            for o in self.objs:
                for addr, i in o.insns.items():
                    here = (o.index, addr)
                    if i.kind in ('plain', 'ccall', 'cond', 'call'):
                        preds[(o.index, addr + 4)].append(here)
                    if i.kind in ('cond', 'jump'):
                        t = self.resolve(o, i.target)
                        if t:
                            preds[t].append(here)
                    if i.kind == 'table':
                        for t in range(i.target[0], i.target[1], 4):
                            preds[(o.index, t)].append(here)
            self.preds = preds
        bit = 1 << reg
        seen, work, out = {key}, list(self.preds[key]), set()
        while work:
            k = work.pop()
            if k in seen:
                continue
            seen.add(k)
            o = self.objs[k[0]]
            i = o.insns[k[1]]
            if i.kind == 'ccall':
                out.add(k)
                continue
            if i.kind == 'call':
                t = self.resolve(o, i.target)
                if t is None:
                    out.add(k)
                    continue
                if not self.returns.get(t):
                    continue
                glo, ghi, _, plo, phi = self.summary[t]
                if (glo | ghi) & bit:
                    out.add(k)
                if not (plo | phi) & bit:
                    continue
            if i.kind == 'plain' and i.def_lo & i.def_hi & bit:
                continue
            work.extend(self.preds[k])
        return out


# What the checker must and must not report, as assembly. Each `_gadget_t_*`
# is one case; `want` names the cases that hold a hazard.
SELF_TEST = r"""
.text
.macro gret
    ldr x8, [x28], #8
    br x8
.endm
.macro g name
    .globl _gadget_t_\name
    .p2align 2
_gadget_t_\name:
.endm

t_helper:                           // reaches C on its slow path
    cbz x0, 1f
    ret
1:  stp x29, x30, [sp, #-16]!
    bl _t_c_function
    ldp x29, x30, [sp], #16
    ret
t_reader:                           // reads v3 it was handed
    str q3, [x1]
    ret
t_tail:
    b _t_c_function

g direct                            // v16 across a C call
    ldr q16, [x1]
    bl _t_c_function
    str q16, [x1, #16]
    gret
g through_helper                    // the MOPS shape
    dup v0.16b, w3
    bl t_helper
    str q0, [x1]
    gret
g hands_on                          // a changed v3 passed to a reader
    ldr q3, [x1]
    bl _t_c_function
    bl t_reader
    gret
g tail_call
    ldr q5, [x1]
    bl t_tail
    str q5, [x1]
    gret
g indirect
    ldr q6, [x1]
    blr x9
    add v7.16b, v6.16b, v6.16b
    str q7, [x1]
    gret
g high_half                         // v8 whole: its high half is not kept
    ldr q8, [x1]
    bl _t_c_function
    str q8, [x1]
    gret
g destructive                       // fmla reads its destination
    ldr q17, [x1]
    bl _t_c_function
    ldr q1, [x1]
    fmla v17.2d, v1.2d, v1.2d
    str q17, [x1]
    gret
g one_lane                          // only the low lane rewritten
    ldr q18, [x1]
    bl _t_c_function
    mov v18.d[0], x9
    str q18, [x1]
    gret
g table                             // the read sits behind a jump table
    ldr q19, [x1]
    bl _t_c_function
    adr x10, 1f
    add x10, x10, x9, lsl #3
    br x10
1:  str q19, [x1]
    gret
    str q19, [x1, #16]
    gret
g loop                              // changed on the way round
    ldr q20, [x1]
1:  str q20, [x1]
    bl _t_c_function
    subs x9, x9, #1
    b.ne 1b
    gret

g ok_reload                         // rebuilt after the call
    dup v0.16b, w3
    bl t_helper
    dup v0.16b, w3
    str q0, [x1]
    gret
g ok_low_half                       // d8 is callee-saved
    ldr d8, [x1]
    bl _t_c_function
    str d8, [x1]
    gret
g ok_two_lanes                      // both lanes written: all of v4
    ldr q4, [x1]
    bl _t_c_function
    mov v4.d[0], x9
    mov v4.d[1], x10
    str q4, [x1]
    gret
g ok_zero
    ldr q5, [x1]
    bl _t_c_function
    eor v5.16b, v5.16b, v5.16b
    str q5, [x1]
    gret
g ok_before                         // read before the call only
    ldr q6, [x1]
    str q6, [x1, #16]
    bl _t_c_function
    gret
g ok_trn2                           // trn2 writes all of its destination
    bl _t_c_function
    ldr q1, [x1]
    trn2 v2.4s, v1.4s, v1.4s
    str q2, [x1]
    gret
"""
SELF_WANT = {'direct', 'through_helper', 'hands_on', 'tail_call', 'indirect',
             'high_half', 'destructive', 'one_lane', 'table', 'loop'}


def self_test():
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'jit_selftest.S')
        obj = src + '.o'
        with open(src, 'w') as f:
            f.write(SELF_TEST)
        subprocess.run(['cc', '-c', src, '-o', obj], check=True)
        o = Obj(obj, 0)
        o.load()
        audit = Audit([o])
        found, _ = audit.analyse()
    cases = {n[len('_gadget_t_'):] for n in o.syms if n.startswith('_gadget_t_')}
    got = set()
    for key, _ in found:
        where = o.where(key[1]).split('+')[0]
        got.add(where[len('_gadget_t_'):])
    ok = True
    for name in sorted(cases):
        want = name in SELF_WANT
        if (name in got) != want:
            ok = False
            print('self-test: %s: %s' % (name, 'not reported' if want
                                         else 'reported, and it is sound'))
    if got - cases:
        ok = False
        print('self-test: reports outside the cases: %s' % sorted(got - cases))
    if ok:
        print('self-test: %d cases, %d hazards reported, %d sound ones not'
              % (len(cases), len(SELF_WANT), len(cases) - len(SELF_WANT)))
    return 0 if ok else 1


def main(argv):
    if '--self-test' in argv:
        return self_test()
    verbose = '-v' in argv
    entry_reads = '--entry-reads' in argv
    args = [a for a in argv if a not in ('-v', '--entry-reads')]
    paths = []
    for a in args:
        if os.path.isdir(a):
            paths += sorted(glob.glob(os.path.join(a, '**', 'jit_*.S.o'),
                                      recursive=True))
        else:
            paths.append(a)
    if not paths:
        print(__doc__)
        return 2
    objs = []
    for n, p in enumerate(paths):
        o = Obj(p, n)
        o.load()
        objs.append(o)
    audit = Audit(objs)
    found, rounds = audit.analyse()

    total = sum(len(o.insns) for o in objs)
    calls = sum(1 for o in objs for i in o.insns.values()
                if i.kind == 'ccall'
                or (i.kind == 'call' and audit.resolve(o, i.target) is None))
    print('%d objects, %d instructions, %d calls straight into C, '
          '%d assembly routines called (%d rounds), %d jump tables'
          % (len(objs), total, calls, len(audit.funcs), rounds,
             sum(o.tables for o in objs)))
    if verbose:
        for f in sorted(audit.funcs):
            glo, ghi, u, plo, phi = audit.summary[f]
            print('  routine %-36s %s changes lo %08x hi %08x, reads on '
                  'entry %08x, keeps lo %08x hi %08x'
                  % (objs[f[0]].where(f[1]),
                     'returns,' if audit.returns[f] else 'no return,',
                     glo, ghi, u, plo, phi))

    if entry_reads:
        # A gadget that reads a vector register it did not write is taking
        # it from the gadget before: state the analysis above cannot see.
        for o in objs:
            for name in sorted(o.externs):
                if name not in o.syms or not name.startswith('_gadget_'):
                    continue
                (_, _, ue, _, _), _, _ = audit.run(
                    [(o.index, o.syms[name])], True)
                if ue:
                    print('  entry read: %s: %s reads %s before writing it'
                          % (o.name, name, ', '.join(
                              'v%d' % r for r in range(32) if ue >> r & 1)))

    merged = defaultdict(int)
    for key, bad in found:
        merged[key] |= bad
    for key in sorted(merged):
        o = objs[key[0]]
        i = o.insns[key[1]]
        regs = [r for r in range(32) if merged[key] >> r & 1]
        if i.kind == 'call':
            t = audit.resolve(o, i.target)
            what = 'calls %s, which reads' % objs[t[0]].where(t[1])
        else:
            what = '`%s %s` reads' % (i.mnem, i.ops)
        print('%s: %s: %s %s after a call that may change it'
              % (o.name, o.where(key[1]), what,
                 ', '.join('v%d' % r for r in regs)))
        for r in regs:
            for src in sorted(audit.origins(key, r)):
                so = objs[src[0]]
                si = so.insns[src[1]]
                tgt = si.target if isinstance(si.target, str) else si.ops
                print('    v%d: %s: %s (%s %s)'
                      % (r, so.name, so.where(src[1]), si.mnem, tgt))
    if not merged:
        print('nothing reported')
    return 1 if merged else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
