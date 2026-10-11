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
| 2 | RUNNINGBOARD 0xdead10cc under heavy fakefs writes | **DONE** (2026-10-11): the gate was lifted before the next assertion was taken; now only under one with time left. The kill itself did not reproduce on demand |
| 3 | The 557 crash in `sockrestart_on_resume` | **DONE** (2026-10-11): poll_wait left a task listed after its exit; one registration per wait now, and a backstop where a task is freed |
| 4 | Caching translated code across exec and across runs | **prototype in, off by default** (2026-10-11): -21..-26% on short processes with addresses held still; needs a decision on ASLR before it can be on |
| 5 | The RISC guests at the threaded-code floor | **profiled, not started** (2026-10-11): the arm64 guest's levers are ALU pairs for 7-Zip and block transitions for Python |
| 6 | `timer_thread_cpu_early` on the A10X and the A9 | **DONE on the A10X** (2026-10-11): a thread's reading of its own CPU time was stale there; the A9 is not to hand to confirm |

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

**Done 2026-10-11.** The crash log names both halves. Thread 27 is a guest
`tar` in `fakefs_open -> db_commit -> sqlite3_step -> pwrite`, a transaction
open; the main thread is in `beginBackgroundTask`, called by the re-arm that
the quiesce's five-second look dispatches. So the gate had just been lifted,
the guest was writing again, and the assertion that should hold iOS off was
still being asked for.

That look (`ISHQuiesceForSuspension`'s `dispatch_after`) lifted the gate from
a background queue and only then, on the main thread, took the next assertion.
Between the two nothing held off the suspension. It runs when the app is
still running five seconds after it quiesced -- or straight away when a
suspended app is resumed in the background, since a block that came due while
frozen fires on wake.

The fix (`ISHQuiesceHoldElapsed`, app/AppDelegate.m) keeps one rule: the gate
is open only in the foreground, while something keeps the app alive, or under
an assertion with time left on it. It takes the assertion first, and lifts
the gate only if `backgroundTimeRemaining` is at least ten seconds (the
expiration handler has to quiesce again, up to two seconds each for fakefs and
the pager). With no time to be had it stays quiesced and looks again in five
seconds. A look scheduled by an earlier quiesce does nothing.

**Seen on the iPhone 12** (a loop of 510 creates and 510 unlinks a pass in
fakefs, the app sent to the background by launching Settings from the Mac,
60 s, eleven times; the app's own log read with idevicesyslog):
- 558's build: suspended inside the five seconds every time it was watched,
  so the look ran only on return. Eight of eight survived: the kill needs the
  suspension to land after the look, and it did not.
- The new build, first trial: `quiesced for suspension` at 10:11:56.50, then
  at 10:12:01.75 `still running after 5s quiesced, with 0.0s of background
  time: staying quiesced`. That is the 558 crash's moment -- running, past
  the five seconds, no background time, the write loop going -- and where the
  old code lifted the gate. The other two were suspended first; all three
  lifted on return `under a new assertion`.

**Not shown:** the kill before and its absence after, as a rate. It did not
reproduce on demand in eight tries; what is shown is the window opening and
the new code holding the gate through it.

## 3. The 557 crash in `sockrestart_on_resume`

**Why.** A field crash (Organizer, 2026-10-07): fs/sockrestart.c:521 writes
`task->sockrestart.punt` through a `listen_tasks` node whose `next` is NULL.

**Done 2026-10-11.** The cause was in `poll_wait` (fs/poll.c). It listed the
waiting task once per socket in the set before the wait and unlisted it once
per socket after, and the wait itself unlocks the set: another thread's
`epoll_ctl` in between left the count wrong for good.

- One socket **removed** during the wait: the count stayed at one, so the
  task stayed on `listen_tasks` after the call, after its exit and after its
  memory was freed. The resume then walked into whatever that memory had
  become -- zeroed, it is the crash in the Organizer (`next` NULL, the write
  at -4); reused by a new task, it is a node linked to itself and a resume
  that never returns. Its neighbours on the list also wrote to the freed node
  whenever they came and went. Any multi-threaded server that edits its epoll
  set from one thread while another sleeps on it does this (Go's netpoller
  does it constantly).
- One socket **added**: the count went negative and the task was never listed
  again, so a resume did not wake it.

The fix: one registration per wait if the set holds a socket
(`sockrestart_begin_wait`/`_end_wait`), which also takes the global
`sockrestart_lock` twice per wait instead of twice per socket. And
`sockrestart_task_gone`, called where a task is freed, unlists a task that is
somehow still listed and says so in the kernel log.

**Proved:** `tests/manual/sockrestart_listeners.sh`'s new `epoll-change` case
(a sleeper on two listeners, one removed by another thread, the sleeper exits,
64 threads take its memory, then the suspend/resume cycle).
- 558's binary: hangs 3 of 3; a host sample shows the resume thread in
  `sockrestart_on_resume` calling `pthread_kill` without end.
- The backstop alone (the old pairing put back): no hang, and the kernel log
  has "pid 3 was freed still listed as waiting on a socket (count 1)", which
  fails the case.
- The fix: passes 3 of 3; all 34 cases of the script pass; 50 poll, epoll and
  socket tests pass on the Devuan arm64 root.

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

**Prototype 2026-10-11** (`ISH_JIT_XCACHE=1`, jit/jit.c "The exec cache";
arm64 guest only, in memory, for the life of the emulator). A translated block
is a function of the guest bytes it came from and the address they sit at --
jit/gen.c reads nothing of the task -- so the cache is keyed by address and a
hit is confirmed by comparing the bytes (the block's own and eight beyond, in
case the translator looked ahead to a fusion it did not take). HLE blocks and
profiled blocks are left out. A hit is a clone, as fork inheritance's is.

Measured on the Mac, -O2 CLI, Devuan arm64 root, cache off and on alternated
five times each, each workload run once to warm and once timed, with
`ISH_RANDOMIZE_VA_SPACE=0` so that a file lands where it did before:

| workload | off (ms) | on (ms) | change |
|---|---|---|---|
| 100 x `/bin/true` | 144 | 111 | -23% |
| 100 x `dash -c :` | 175 | 129 | -26% |
| 30 x `perl -e 1` | 174 | 138 | -21% |
| 3 x gcc compile and link | 1130 | 985 | -13% |
| 100 x `echo x \| sed q \| tr a b` | 653 | 593 | -9% (noisy) |
| 5 x `gcc -O1 -c` | 1208 | 1271 | no change (ranges overlap) |

97% of the blocks of the 100 `/bin/true` run were hits (174,834 of 180,390),
from 2.4 MB of cache. The Devuan arm64 suite passes with the cache on (363,
less `aslr_layout`, which is the test of the ASLR that was turned off for it).

**The decision it waits on.** The stream holds absolute guest addresses, so a
block is reusable only where its code is mapped at the same address, and the
guest's ASLR moves every image on every exec. Three ways on:

1. **One base per file per boot.** The kernel puts a given file at the same
   address in every process until the app restarts; stack, heap and anonymous
   mappings stay random per exec. Small (exec.c's PIE and ld.so placement, and
   mmap's choice for a file mapping with no hint), and the cache then works as
   it is, for every guest. The cost is ASLR's: a library's address leaked from
   one process is its address in the others. It is what Windows does for
   images and what iOS does for its own shared cache.
2. **Rebase the blocks.** Every emitter that puts a guest address in the
   stream tags it (the July plan's `gen_addr`), per backend, with a paranoid
   mode that translates and compares. Keeps ASLR as it is; weeks of work in
   15,000 lines of emitters, and a missed site is silently wrong code.
3. **Leave it as an option** for roots run with `randomize_va_space` 0.

Recommended: 1, behind a setting, measured on the devices before it is the
default. Then the other three guests (riscv64's translation also depends on
the vector type in force; x86's on mode and segment state -- each needs the
"function of bytes and address" claim checked before its blocks are shared), a
bound and eviction, and a flush when what the translator emits is changed at
run time (`/proc/ish/arm64_jit_fuse`, `hle`).

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

**Profiled 2026-10-11** (ISH_JIT_PROFILE, arm64 guest, Alpine 3.24, the Mac's
-O2 CLI; tools/jitprof-report.py). What is fused today: cmp + b.cond, load +
cmp, load/store pairs, the return cache (`/proc/ish/arm64_jit_fuse`).

- **7-Zip** (`7z b 1 -mmt1 -md22`, 27.3G instructions, 27.9 a block): 48%
  register data processing, 21% immediate, 17% loads and stores, 14% branches.
  The commonest pairs are ALU on ALU, none of them fused: add -> eor 7.5% and
  eor -> add 7.5%, add -> add 2.7%, mul -> cmp 2.3%, sub -> sub 1.4%, lsr ->
  lsl 1.4%, lsl -> mul 1.4%, add -> sub 1.3%. cmp + b.cond, already fused, is
  9.4%. So the first superinstruction to try is a pair of register ALU ops in
  one gadget, operands fed as the ospec gadgets take them.
- **Python** (tests/bench-ab/pybench.py, 4.0G instructions): 37% loads and
  stores, 27% immediate, 20% branches, and **8.1 instructions a block** -- a
  block transition every eighth instruction. Pairs: ldr -> ldr 4.6%, adrp ->
  ldr 2.2%, mov -> mov 2.2%, ldr -> cmp 2.0% (fused), str -> str 2.0%. Here
  the lever is the transition itself (the queue's item 3: chain_ip's poke
  check and the chain-budget load and store on every chained branch), then
  ldr pairs.

Not done yet: the 557 -> 558 slips (item 1 of the order above), which need the
two app builds alternated on the devices, and the riscv64 guest's own profile.

## 6. `timer_thread_cpu_early` on the A10X and the A9

**Why.** A red test in every device leg on two devices, on 557 and 558 alike:
a CLOCK_THREAD_CPUTIME_ID timer armed for 5.0 ms of CPU fires with 0.5-4.8 ms
used by the thread's own clock.

**Done 2026-10-11.** The suspicion in the queue (two clocks that disagree) was
half right, and a probe on bip said which half. Three views of one thread's
CPU time, side by side over a burst-and-nap loop: its own
CLOCK_THREAD_CPUTIME_ID, its own `thread_info` (getrusage RUSAGE_THREAD), and
`thread_info` asked from another thread while it naps.

- The view from another thread agrees with the thread's clock: 0.01-0.26 ms
  apart, the same total over 3 s. So the timer thread's samples were fine.
- The thread's `thread_info` of ITSELF leaves out the run on the CPU it is in
  the middle of: 4.06 ms behind its clock after a 4 ms burst, 6.9 ms after a
  9 ms one. The M4 (iPadOS 26) and macOS read the same from both.
- A timer takes "now" on the arming thread. The test spins 1-9 ms and arms, so
  the anchor was that much in the past and the 5 ms timer fired that early.

The fix (kernel/task.c `host_thread_cpu_ns_self`): a thread's reading of its
own CPU time is topped up to its own clock, in `task_thread_cpu_time_ns` (the
timer's anchor) and in `cpu_usage_self` (getrusage, times(), the process
clock's share for the calling thread).

**Proved on bip (A10X):** `timer_thread_cpu_early` passes 10 runs of 10 (it
failed 27-30 rounds of 30 on 557 and 558); the probe now shows the thread's
own two views 0.004-0.024 ms apart after a 9 ms burst. The M4 iPad still
passes it, 3 runs of 3. The Mac passes it with
timeout_and_cpu_timer, rusage_monotonic, proc_pid_stat_cputime, itimer_prof_rate
and getrusage_group. **Not run:** the A9 (ip5), which was not connected -- the
same kernel generation and the same symptom, so expected to follow.

**Found beside it, queued in docs/TODO.md:** a POSIX timer on
CLOCK_PROCESS_CPUTIME_ID runs on the whole emulator's CPU clock. It is why
`timer_missed_periods` fails on bip whenever its process-clock case is not
skipped, and it fails on the Mac on 558 too once another guest process is busy.

---

## Wanted if there is room

Not musts. Each is in docs/TODO.md's queue: nullDescriptor in the MoltenVK fork
(GL on the GPU for Mesa 25.2+, and Wayfire on Alpine 3.24); guest fault reports
flooding the kernel log; GH #631, ssh sessions dropping when the device idles;
a CLOCK_PROCESS_CPUTIME_ID timer running on the whole emulator's CPU (found
with §6).

## Done beside the musts

- **The native applet that ran under the wrong name** (2026-10-11). It
  happened a second time, on the same host and the same line, during this
  cycle's own suite run. Darwin's `basename()` answers from one buffer for the
  whole process, native programs are threads of that process, and SmallCLUE
  picks its applet with `basename(argv[0])`: an applet starting beside another
  could read the other's name. `tests/manual/native_applet_race.c` starts
  `true` and `false` together 4000 times: one wrong in every run of three
  before, none in 12,000 after. basename, dirname, strtok, gmtime and asctime
  are per thread for native programs now (kernel/native_libc.c), and so are
  the shim's own static buffers.
- **Native `passwd`** said "not supported on this platform" (reported
  2026-10-11): the one shadow-auth applet still compiled for a Linux host
  only. Fixed in SmallCLUE; native_coreutils checks it is there.
- **iPhone 18 Pro and Pro Max** in kernel/hostinfo.m, and
  `tools/check-device-table.py`, which compares the table with Xcode's device
  database (nothing else was missing).
