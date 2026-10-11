# build 559 musts

Work that must be done, or explicitly decided, before 559 is tagged. Each
entry says what is **established**, what the **next step** is, and how to
**prove** it afterwards.

Started 2026-10-11, after `752bbdddb` (build: 558). Supersedes
[docs/build_558_musts.md](build_558_musts.md). The detail behind each entry is
in [docs/TODO.md](TODO.md)'s queue; this file is the list and its state.

**Status, 2026-10-11:**

| § | item | state |
|---|---|---|
| 1 | Vector registers held across a call that can reach C | **DONE** (2026-10-11): no other gadget has it; a checker now reads the built objects in `meson test` and the gate |
| 2 | RUNNINGBOARD 0xdead10cc under heavy fakefs writes | open |
| 3 | The 557 crash in `sockrestart_on_resume` | open |
| 4 | Caching translated code across exec and across runs | open |
| 5 | The RISC guests at the threaded-code floor | open |
| 6 | `timer_thread_cpu_early` on the A10X and the A9 | open |

Order: corruption and crashes first (1-3), then the two speed items (4, 5),
which share their measuring kit, then 6.

---

## 1. Vector registers held across a call that can reach C

**Why.** 558's blocker (a4e9b84fc): the arm64 MOPS SET gadget kept its fill
pattern in v0 across `a64_xlate_w`, and on an A10X host the TLB slow path now
and then came back with v0 changed -- gcc died one compile in five, Wayfire at
boot, and Python spun for hours on the iPhone 12. No M-series host showed it.
`save_c`/`restore_c` keep x0-x18 and lr only, and the AAPCS64 lets a callee
change all of v0-v7 and v16-v31 and the high halves of v8-v15, so any gadget
with the same shape is the same bug waiting for a host that exercises it.

**Established:** `a64_xlate_r/w`'s other caller (`a64_vldst`) holds no vector
register, and the MOPS copies load and store with no call between.

**Done 2026-10-11:** `tools/gadget-vreg-audit.py` reads the assembled gadget
objects rather than the macro sources. It disassembles every `jit_*.S.o`,
builds the control flow (in-gadget jump tables included), and carries "a C
call may have changed this half" forward over v0-v31, low and high halves
apart -- a callee keeps v8-v15's low halves, and two 64-bit lane writes make a
whole register. A call to another assembly routine goes through a summary of
that routine, so the C call is found wherever it sits.

- **Result: nothing else has the bug.** 1,968,006 instructions in 31 objects,
  40,525 gadgets, 126 calls straight into C, 102 assembly routines, 28 jump
  tables: no read of a changed register, in the Mac build and in 558's own
  Release-iphoneos archive objects. The x86 VEX/EVEX, x87, riscv64 vector and
  arm64 SIMD/FP gadgets all rebuild or reload around their slow paths.
- **Positive control:** with `jit/guest-arm64/control.S` from before a4e9b84fc
  it reports `_gadget_arm64_mops+0x2a8: calls mops_fill, which reads v0`, from
  `bl a64_xlate_w` at `+0x29c` -- the 558 blocker, by name.
- **Self-test** (`--self-test`): sixteen small gadgets, ten with the hazard in
  a different shape each (straight call, through a helper, handed to a reader,
  tail call, `blr`, v8's high half, a destructive op, one lane rewritten,
  behind a jump table, round a loop) and six sound ones it must leave alone.
- **No gadget hands vector state to the next one:** no gadget reads a vector
  register before writing it on its straight-line entry. (`--entry-reads` lists
  the may-reads; they are pairs of branches on one flag, load-if/use-if.)
- **Kept as a check:** `meson test gadget_vreg_selftest gadget_vreg_audit` on
  an Apple silicon host (16 s), and a leg of `tools/run-guest-gate.sh`.
- The host backend for x86_64 (`jit/gadgets-x86_64`, Linux CI only) uses no
  vector register at all.

Not chased: which callee changes v0 on the older cores only. The rule holds
whatever the answer, and the checker enforces the rule.

## 2. RUNNINGBOARD 0xdead10cc under heavy fakefs writes

**Why.** The commonest crash signature in the Organizer over 549-555, and the
558 candidate still died of it on the iPhone 12 (2026-10-10): the phone locked
while a suite compiled and a cache push wrote into /AOK/fakefs, and iOS killed
the app in `ISHQuiesceForSuspension -> ISHRearmSuspendGuardIfBackgrounded ->
ISHBeginSuspendGuardAssertion`.

**Established:** 556's quiesce fix (6de50726) closed the DNS-refresh and
lapsed-gate cases. This one is a write-heavy guest at lock.

**Next step:** reproduce with a write loop into fakefs and a screen lock; name
the thread that holds the SQLite lock when the assertion lapses; make the
quiesce wait for (or refuse) writers before the app is suspended.

**Prove:** the repro survives N lock/unlock cycles on a device, and
/proc/ish or the log shows the quiesce reached with no fakefs transaction open.

## 3. The 557 crash in `sockrestart_on_resume`

**Why.** A field crash (Organizer, 2026-10-07): fs/sockrestart.c:521 writes
`task->sockrestart.punt` through a `listen_tasks` node whose `next` is NULL.

**Established:** nothing removes a task from `listen_tasks` at teardown, and
poll.c pairs begin/end over a `poll_fds` list another thread's `epoll_ctl` can
change in between. A CLI probe of that shape did not crash.

**Next step:** remove the task at exit and assert its count is zero; then a
probe that changes the epoll set during a poll across a suspend.

**Prove:** the probe fails on 558 and passes after; the socket and epoll tests
in the five-root suite.

## 4. Caching translated code across exec and across runs

**Why.** Every exec starts with no translations, so each process re-translates
ld.so, libc and its program. Measured 2026-10-01: translation is ~19-23% of
CPU for `sh -c true` and fork+exec loops, ~8-10% for configure scripts and
Python start-up, ~3-5% for compiles. Fork inheritance (9e28a5f9) took the
fork-without-exec share; only a cache keyed by file reaches the rest.

**Established:** a cached block must still be found, copied and rebased
(gadget addresses change per launch, guest addresses per ASLR): maybe 50-100 ns
against 250 ns (M4) to 1 us (A10X) to translate, so the cache recovers roughly
60-90% of those shares. [jit_code_cache_plan.md](../jit_code_cache_plan.md) (repo root) is the July Phase 0.

**Next step:** design the key (file identity + offset + page content), the
relocation records a block needs, and where the cache lives (in memory across
exec first; on disk second). Build the in-memory one and measure it with the
2026-10-01 driver before deciding on persistence.

**Prove:** interleaved A/B on the M4 and the A10X for the 2026-10-01 workloads;
the five-root suite with the cache on; SMC and `mprotect`/`munmap` tests
(x86_smc, the exec/TLB tests) since a stale cached block is silent wrong code.

## 5. The RISC guests at the threaded-code floor

**Why.** 557 -> 558 took x86_64 from 165 to 817 on the M4's 7-Zip and left
arm64 (997 -> 979) and riscv64 (913 -> 886) where they were: ~1.5-2 ns a guest
instruction, the cost of a dispatch.

**Next step, in the queue's order:** (1) the small slips that held across every
round (riscv64 7-Zip -3%, arm64 Python str +7%, riscv64 method +8%) -- they may
name one gadget change; (2) profile-driven superinstructions from the hottest
gadget pairs and triples; (3) cheaper block transitions (measure first: the
lost-poke history, c5692489); (4) guest registers resident across chained
blocks.

**Prove:** tests/bench-ab on the M4 and the A10X, builds alternated; the
arm64 and riscv64 suites.

## 6. `timer_thread_cpu_early` on the A10X and the A9

**Why.** A red test in every device leg on two devices, on 557 and 558 alike:
a CLOCK_THREAD_CPUTIME_ID timer armed for 5.0 ms of CPU fires with 0.5-4.8 ms
used by the thread's own clock.

**Established:** the timer samples `thread_info(THREAD_BASIC_INFO)` from the
timer thread while the guest reads the host's own-thread clock, and on those
older kernels the two disagree by milliseconds. The M4 and the iPhone 12 pass.

**Next step:** THREAD_EXTENDED_INFO's nanosecond fields, or anchor the arming
to thread_info's figure rather than the clock's.

**Prove:** the test on bip and ip5, 30 rounds each; still passing on the M4.

---

## Wanted if there is room

Not musts. Each is in docs/TODO.md's queue: nullDescriptor in the MoltenVK fork
(GL on the GPU for Mesa 25.2+, and Wayfire on Alpine 3.24); guest fault reports
flooding the kernel log; the native applet that ran under the wrong name; GH
#631, ssh sessions dropping when the device idles.
