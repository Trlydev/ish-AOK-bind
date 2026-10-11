# AOK's CPU engine inside UTM SE: speed race and plug-in study

Status: **the RISC-V plug-in is built and working (2026-10-11); see section
8.** Sections 1 to 7 are the study that led to it, as written before any of
it existed. It answered two questions that came out of a Discord suggestion to
run a real Linux kernel:

1. Is AOK's gadget engine enough faster than the engine UTM SE ships to be
   worth putting inside UTM SE?
2. If so, where does it plug into QEMU, and what exactly would it have to
   provide?

The goal behind both: boot unmodified Linux, FreeDOS and old Windows on an
iPad or iPhone, at better than UTM SE speed, and offer the work to UTM as a
pull request rather than ship a competing app.

## Summary

- **AOK's engine is much faster than the one UTM SE ships.** For identical
  programs on the same machine, UTM SE's engine needs **7.8x the CPU cycles
  on an arm64 guest and 6.5x on a riscv64 guest** (geometric mean of six
  workloads; range 3.1x to 14.5x). The ratio is the same on the Mac's fastest
  cores and on its mid-tier cores.
- **It is still about 3x ahead of the best version of that engine.** An
  active fork, `tctiSH/qemu` (QEMU 11.1), has made UTM SE's engine about 2.4x
  faster. Against it AOK leads by **3.3x on arm64 and 2.8x on riscv64**
  (range 1.6x to 4.5x). That fork, not UTM's QEMU 10, is the base to build
  on: UTM plans to move to QEMU 11.1 (section 1, "The moving target").
- **AOK is within about 1.5x of QEMU's real JIT** on the same workloads, and
  ahead of it on floating point. The JIT is what UTM uses where Apple allows
  code generation; an App Store app cannot.
- **A full VM still pays for an emulated kernel.** Whatever the engine,
  syscall-heavy work will be slower in a VM than in AOK today, where the
  kernel is native code.

- **The plug-in now exists for RISC-V guests, and delivers most of that
  lead** (section 8). Inside QEMU it needs 1.5x to 2.9x fewer host cycles
  than the tctiSH fork's engine on the same six workloads, and 2.9x to 7.2x
  fewer than UTM SE as shipped, with program output and floating-point
  results identical to QEMU's own.

- **Plug-in point: a second block generator inside QEMU's existing engine,
  not a new accelerator.** UTM SE's engine and AOK's use the same dispatch
  scheme, down to the host register that holds the stream pointer. A block
  AOK generates can live in QEMU's translation cache beside the blocks QEMU
  generates, be entered and left the same way, and call QEMU's own C helpers
  for everything privileged. AOK would not have to implement kernel mode, the
  MMU, interrupts or any device.
- **Every block AOK declines falls back to QEMU's own translator.** That makes
  the work incremental: day one it handles nothing and UTM SE behaves exactly
  as it does today; each instruction class added afterwards makes it faster.
- **x86 guests (FreeDOS, Windows) keep QEMU's engine** until someone ports
  AOK's x86 gadgets the same way. They lose nothing in the meantime.
- **The shipped engine has a defect the fork has already fixed**: in QEMU 10
  it makes a C call for every guest memory access, because its inline fast
  path only covers table offsets that current QEMU no longer uses (section 2).
  Nothing to contribute there.
- **UTM accepts AI-assisted pull requests**, under rules listed in section 5.

## 1. Speed race

### Rig

| | |
|---|---|
| Host | MBP5.local, Apple M5 Max (6 Super + 12 Performance cores), macOS 26 |
| AOK | `build/ish` CLI at `54f2b227`, `debugoptimized` (-O2), default settings (HLE off) |
| UTM SE's engine | UTM's QEMU 10.0.12 (`utmapp/qemu` release tarball), `--enable-tcg-threaded-interpreter` -- the flag UTM's `build_dependencies.sh` uses for its `ios-tci` platform. QEMU's configure calls it "TCTI (TCG with threaded-dispatch bytecode interpreter, experimental and slow; but faster than TCI)" |
| QEMU's JIT | the same source configured without that flag. Not available to App Store apps; here as the ceiling |
| Guests | arm64 (`-M virt -cpu cortex-a72`, UTM's default) and riscv64 (`-M virt -cpu rv64`), one CPU, 2 GB |
| Kernel under QEMU | Alpine 3.24's stock `linux-virt` / `linux-lts` 6.18.55, unmodified |
| Userland | one Alpine 3.24.2 tree per architecture, built once. AOK runs it as a fakefs root; QEMU boots the *same files* as an initramfs. Every program on both sides is byte-identical |

Workloads, each a single process: CoreMark (4000 iterations), a
double-precision n-body simulation (2M steps), a pure-Python mix, `xz -3` of
2 MB of text, busybox `sha256sum` of 16 MB, and a busybox `awk` word count
over 1 MB.

### Method, and why it is not wall-clock time

The first attempt timed each workload from inside the guest. On this Mac the
same AOK binary on the same input then varied **3x** between runs minutes
apart: the machine was also running a local LLM and throttling, and macOS
moved the benchmark between the Super cores at 4.3 GHz and the mid-tier cores
at about 1.4 GHz. Ratios taken that way were wrong by up to 3x per row,
because the two engines being compared had run in different states.

So the figures below are **host CPU cycles**, with host instructions beside
them. A small wrapper (`cycwrap`) starts the engine, watches its output, and
reads the process's cycle and instruction counters (`proc_pid_rusage`) when the
guest prints a start marker and again when it prints the result line. Nothing
is subtracted: a QEMU guest's boot happens before the first marker. Cycles do
not move with clock speed or with time-slicing. They do move with the core
class a run lands on (AOK's CoreMark costs 4.0 G cycles on a Super core and
4.7 G on a mid-tier one), so the wrapper also records what share of each
sample's instructions ran on the Super cores, and samples are sorted into
"Super" (at least 90%), "mid-tier" (at most 10%) and discarded (16 of 216
straddled the two). Six rounds were run, strictly one process at a time,
while the machine moved between the two states on its own.

### Results

Billions of host CPU cycles to finish the same work; lower is better. "UTM SE
needs" is UTM SE's engine divided by AOK. The last column repeats that ratio
for samples that ran on the mid-tier cores while the machine was throttled.

**arm64 guest**

| Workload | AOK | UTM SE's engine | QEMU's JIT | UTM SE needs | AOK vs JIT | UTM SE needs (mid-tier cores) |
|---|--:|--:|--:|--:|--:|--:|
| CoreMark | 4.0 | 20.2 | 1.6 | 5.1x | 2.4x | 5.4x |
| n-body (floating point) | 2.7 | 24.5 | 4.7 | 9.1x | 0.6x | 7.8x |
| Python | 7.0 | 90.9 | 5.2 | 13.0x | 1.4x | 13.1x |
| xz | 3.6 | 28.9 | 2.7 | 8.1x | 1.3x | 8.5x |
| sha256sum | 2.5 | 8.2 | 0.7 | 3.3x | 3.6x | 3.9x |
| awk | 2.5 | 35.9 | 2.0 | 14.6x | 1.2x | 17.1x |
| Geometric mean | | | | **7.9x** | 1.5x | 8.2x |

**riscv64 guest**

| Workload | AOK | UTM SE's engine | QEMU's JIT | UTM SE needs | AOK vs JIT | UTM SE needs (mid-tier cores) |
|---|--:|--:|--:|--:|--:|--:|
| CoreMark | 4.6 | 21.4 | 2.6 | 4.6x | 1.8x | 4.9x |
| n-body (floating point) | 3.2 | 22.5 | 5.1 | 7.1x | 0.6x | 6.4x |
| Python | 8.6 | 92.2 | 5.9 | 10.8x | 1.5x | 10.4x |
| xz | 4.3 | 21.7 | 2.9 | 5.0x | 1.5x | 5.6x |
| sha256sum | 3.1 | 9.6 | 1.0 | 3.2x | 3.2x | 2.9x |
| awk | 2.8 | 36.5 | 2.6 | 13.1x | 1.1x | 15.1x |
| Geometric mean | | | | **6.5x** | 1.4x | 6.6x |

In seconds on the fastest cores at 4.3 GHz, arm64 guest (AOK / UTM SE's
engine / QEMU's JIT): CoreMark 0.93 / 5.0 / 0.38; n-body 0.63 / 6.1 / 1.10;
Python 1.62 / 26.1 / 1.20; xz 0.82 / 6.7 / 0.63; sha256sum 0.57 / 1.9 / 0.16;
awk 0.57 / 8.4 / 0.48.

Each cell is the median of one to four samples in that machine state (most
have two or three); within a state, repeat samples agree to a few percent.
Counting host *instructions* instead of cycles gives smaller ratios, 2.2x to
8.6x, because UTM SE's engine also runs fewer instructions per cycle than AOK
(for Python on arm64, 2.1 against 3.9).

### The moving target: the tctiSH fork

UTM's tracker (utmapp/UTM#7930, opened 2026-09-30) points at
`github.com/tctiSH/qemu`, "a fork of QEMU that plays host to the TCTI
pseudo-JIT": QEMU 11.1.2 with dozens of recent commits to this engine. Among
them are fused gadgets for loading and storing guest state, a memory fast
path for every privilege level, in-stream indirect jumps (`goto_ptr`), a
larger block-lookup cache, and host floating point for x86 guests. Its author
offered it to UTM; UTM's maintainer answered that QEMU 11.1 is planned for
UTM 5.1 and he will revisit the engine then. He is also considering a
WebAssembly backend.

The same race against that fork (`ded0cff`, built the same way), fastest
cores, billions of host cycles:

**arm64 guest**

| Workload | AOK | tctiSH fork | UTM SE today | fork needs | UTM SE today needs | fork vs UTM SE today |
|---|--:|--:|--:|--:|--:|--:|
| CoreMark | 4.0 | 10.7 | 20.2 | 2.7x | 5.1x | 1.9x faster |
| n-body (floating point) | 2.7 | 10.5 | 24.5 | 3.9x | 9.0x | 2.3x faster |
| Python | 7.1 | 32.0 | 90.9 | 4.5x | 12.8x | 2.8x faster |
| xz | 3.6 | 11.0 | 28.9 | 3.1x | 8.1x | 2.6x faster |
| sha256sum | 2.5 | 4.5 | 8.2 | 1.8x | 3.3x | 1.8x faster |
| awk | 2.5 | 11.0 | 35.9 | 4.4x | 14.5x | 3.3x faster |
| Geometric mean | | | | **3.3x** | 7.8x | 2.4x faster |

**riscv64 guest**

| Workload | AOK | tctiSH fork | UTM SE today | fork needs | UTM SE today needs | fork vs UTM SE today |
|---|--:|--:|--:|--:|--:|--:|
| CoreMark | 4.6 | 11.8 | 21.4 | 2.6x | 4.6x | 1.8x faster |
| n-body (floating point) | 3.2 | 10.1 | 22.5 | 3.2x | 7.1x | 2.2x faster |
| Python | 8.6 | 33.4 | 92.2 | 3.9x | 10.8x | 2.8x faster |
| xz | 4.4 | 9.5 | 21.7 | 2.2x | 5.0x | 2.3x faster |
| sha256sum | 3.1 | 5.0 | 9.6 | 1.6x | 3.1x | 1.9x faster |
| awk | 2.8 | 11.4 | 36.5 | 4.1x | 13.1x | 3.2x faster |
| Geometric mean | | | | **2.8x** | 6.5x | 2.3x faster |

All of these samples ran on the Super cores at about 4.3 GHz (three per
cell for the fork, five or six for AOK). The fork is improving week by week,
so this gap may narrow further; the part it cannot remove without changing
design is one dispatch per intermediate operation.

### Reading the numbers

- **The lead is largest where code is branchy and touches memory** (Python,
  awk, xz) and smallest on long runs of register arithmetic (sha256sum).
  Section 2 explains why.
- **AOK beats QEMU's JIT on floating point** because AOK runs guest FP on the
  host's FP unit and QEMU computes it in software.
- **awk is partly a kernel measurement.** Under QEMU it spends a large share
  of its time inside the guest kernel (41% under the JIT in an early run),
  serving memory-allocation syscalls. Under AOK the same syscalls are native
  C and took 0.5%.
- **These are AOK's numbers as a user-mode emulator with its own kernel.**
  Inside QEMU the same gadgets would use QEMU's software TLB and run the
  guest kernel too. The per-instruction cost should carry over; it has not
  been measured.

### What this does not measure

- **AOK in kernel mode.** AOK has none today. Under QEMU the guest kernel's own
  instructions run through the engine, so syscall-heavy and page-fault-heavy
  work pays for them; under AOK they are native C. `awk` is the one workload
  here where that shows.
- **An iPad or iPhone.** The ratio between two engines that use the same
  dispatch scheme on the same core should carry over, but it has not been
  measured there.
- **x86 guests.** UTM SE's engine does more work per x86 instruction (flags,
  segments) than per arm64 or RISC-V instruction; AOK's x86 engines were not
  raced.
- **Devices.** Disk, network and display emulation are QEMU's either way.

## 2. How UTM SE runs guest code

UTM is an Apache-2.0 front end around QEMU. It builds QEMU 10.0.12 from
`utmapp/qemu` plus one patch that touches display, virtual-GPU and hypervisor
code, not the CPU engine.
On iOS without JIT ("UTM SE", platform `ios-tci`) it configures QEMU with
`--enable-tcg-threaded-interpreter`, which selects the back end in
`tcg/aarch64-tcti/` (about 4,800 lines; not in mainline QEMU).

QEMU's engine (TCG) works in two halves:

1. A per-guest **front end** (`target/riscv`, `target/arm`, `target/i386`)
   turns a block of guest instructions into QEMU's intermediate operations.
   This is also where all the kernel-mode behaviour lives: privilege checks,
   control registers, page-table walks, exceptions. It is large: about 34,000
   lines for RISC-V and 88,000 for ARM.
2. A per-host **back end** turns those operations into something runnable.
   Normally that is machine code. TCTI instead emits a stream of *addresses of
   precompiled gadgets*, one per operation, each gadget ending in "load the
   next address, jump to it".

That is the same scheme as AOK's JIT, at a different granularity: TCTI
dispatches once per intermediate operation, AOK once per guest instruction.
Facts from the source that matter for plugging in:

| | UTM SE's engine (TCTI) | AOK |
|---|---|---|
| Dispatch | `ldr x27, [x28], #8; br x27` | the same, `_ip` is `x28` |
| Guest CPU state | pointer in `x14` | pointer in `x1` (`_cpu`) |
| Guest registers | cached in host `x0`-`x15` within a block, written back at block ends and before C calls | read and written in the state structure by each gadget |
| Guest memory | inline lookup in QEMU's software TLB (`addr_read`, `addr_write`, `addend`), C helper on a miss | inline lookup in AOK's TLB (`page`, `page_if_writable`, `data_minus_addr`), C on a miss |
| Direct branches | a slot in the stream, patched when the target block exists | chained blocks |
| Indirect branches, returns | **leave the stream and go back through QEMU's C main loop** to find the next block (TCTI has no `goto_ptr`) | a target cache looked up inside the gadget (`jit/guest-arm64/control.S`) |
| Floating point | QEMU's software float library through C helpers | host FP instructions |
| Gadget count | one per operation *per register combination*: the binary is 320 MB | one per instruction form |

A profile of UTM SE's engine running the Python workload (`sample`, 8 s,
arm64 and riscv64 guests alike) puts its active time at roughly:

| Share | Where |
|--:|---|
| 64% | ordinary gadgets, a large part of them loading and storing guest registers and constants |
| 23% | guest memory access through the **slow path**: a C call per load or store |
| 10% | returning to QEMU's main loop to find the next block |
| 3% | other |

The memory figure is a defect rather than a design cost. TCTI has inline
TLB-lookup gadgets only for five fixed offsets of the TLB table (-32, -48,
-64, -96, -128 from the CPU state; `LOOKUP_SPECIAL_CASE_LDST_GADGET` in
`tcg-target.c.inc`), which matched common privilege levels in older QEMU.
In QEMU 10 every guest has 16 slots, the ones arm64 and riscv64 guests use
fall outside those five, and every access takes the `_slowpath_` gadget and a
C helper. No fast-path memory gadget appeared in either profile.

## 3. Where AOK's engine could plug in

### A. As a QEMU accelerator (`-accel aok`): not recommended

This is the interface KVM and Apple's Hypervisor.framework use
(`include/system/accel-ops.h`: create and kick CPU threads, synchronise
register state, deliver interrupts). It is small, but it assumes the
accelerator *is* a complete CPU. Hardware virtualisation gets kernel mode from
the real processor. AOK would have to supply it: control registers, the
page-table walker, exception entry and return, timers, for every guest
architecture. QEMU already has all of that, but inside its own engine, where
an accelerator cannot reach it. The existing glue gives the scale of what
would be left to write even *with* a CPU that does kernel mode itself:
`target/arm/hvf/hvf.c` is 2,557 lines and `target/riscv/kvm/kvm-cpu.c` 2,178,
and device code carries a few dozen `kvm_enabled()` special cases that a new
accelerator would have to add to.

### B. As a second block generator inside QEMU's engine: recommended

QEMU builds a block in `setjmp_gen_code()` (`accel/tcg/translate-all.c`): the
guest's front end produces intermediate operations, then `tcg_gen_code()` runs
the back end. The hook goes in front of that pair: offer the block to AOK's
generator first; if it returns a stream, use it; if it declines, carry on as
today. The block is the same `TranslationBlock` either way, in the same cache,
entered through the same `tcg_qemu_tb_exec()`.

What that buys:

- **No kernel mode to write.** Privileged and rare instructions are calls to
  the C helpers QEMU already has (`helper_csrrw`, `helper_sret`, `helper_wfi`,
  about 1,180 helper declarations for RISC-V), or simply a declined block.
- **No MMU to write.** Gadgets look addresses up in QEMU's software TLB and
  call QEMU's miss helpers, which walk the guest's page tables, raise faults,
  reach devices and catch writes to translated code.
- **No interrupt or device work.** Unchanged.
- **Incremental and safe to ship half-done.** Anything unhandled is a declined
  block.
- **A contained diff**: a few dozen lines in `accel/tcg`, a new directory for
  the generator and its gadgets.

What it costs: AOK's gadgets and decoder for that guest have to be retargeted
to QEMU's data structures (section 4), and the result is tied to QEMU
internals that change between QEMU releases. UTM pins its QEMU version
(10.0.12 today), so the rebase comes when UTM moves.

### C. Improving TCTI in place

Some of AOK's advantage can be had with no new front end at all, by fixing
the worst costs in TCTI itself (the slow-path memory access above, and the trip back to the main loop on every indirect branch). The tctiSH fork has done exactly this, and
more, and got about 2.4x (section 1). It helps every guest, x86 included.
What remains is the roughly 3x that comes from the design itself: dispatching
per intermediate operation rather than per guest instruction.

## 4. What AOK's engine would have to provide (option B)

The generator is called with the guest CPU state, the block's start address,
and the block's flags. It must produce the following, or decline.

| # | Requirement | Where QEMU defines it | AOK today |
|---|---|---|---|
| 1 | **A gadget stream in QEMU's code buffer**, entered with the state pointer in `x14` and the stream pointer in `x28` | `tcg_qemu_tb_exec()` in `tcg/aarch64-tcti/tcg-target.c.inc` | Same dispatch; needs an entry gadget that moves `x14` to `_cpu`, and gadgets built against QEMU's state layout |
| 2 | **Guest state at QEMU's offsets**: `CPURISCVState.gpr[]`, `fpr[]`, `pc`, vector registers; `CPUARMState` for arm64 | `target/*/cpu.h` | Offsets come from `jit/offsets.c`; regenerate against QEMU's structures |
| 3 | **Memory access through QEMU's software TLB**, per privilege level (`mmu_idx` is fixed per block); on a miss call `helper_ld*_mmu` / `helper_st*_mmu` | `include/exec/tlb-common.h`, `accel/tcg/cputlb.c` | Two macros per guest (`load_gadget`, `store_gadget` in `jit/guest-riscv64/memory.S`); the entry layouts are near-identical |
| 4 | **Honour the block's flags or decline**: privilege level, FP and vector enable state, address width, and so on | `TB_FLAGS` in `target/riscv/cpu.h` (22 fields); `TBFLAG_A64` in `target/arm/cpu.h` (37 fields) | New. AOK's generators assume user mode with everything enabled. Start by declining any block whose flags are not the plain case |
| 5 | **Check for a pending exit at block entry** (`icount_decr`), returning `TB_EXIT_REQUESTED` | `accel/tcg/translator.c` | AOK has an equivalent poke check; different field |
| 6 | **Leave the block the way QEMU expects**: return `block pointer | exit index`; direct jumps through `jmp_target_addr[]` slots that QEMU patches and resets | `tb_target_set_jmp_target()`, `tcg_out_goto_tb()` | Replaces AOK's own chaining for these blocks |
| 7 | **Indirect branches**: at minimum exit to the main loop, as TCTI does; better, an in-gadget lookup against QEMU's per-CPU jump cache | `tb_lookup()` in `accel/tcg/cpu-exec.c`, `tb-jmp-cache.h` | AOK's return and indirect-target cache has to be re-pointed at QEMU's |
| 8 | **Precise state when a helper faults**: per-instruction records (`gen_insn_data`, `gen_insn_end_off`) so QEMU can recover the guest PC from a stream position | `encode_search()`, `cpu_restore_state()` | New bookkeeping in the generator; TCTI shows the mechanism (`tcti_call_return_address`) |
| 9 | **Read guest code through QEMU** so a block that crosses a page is tracked and writes to it invalidate the block | `translator_ld*`, `tb_set_page_addr1()` | Replaces AOK's own self-modifying-code tracking for these blocks |
| 10 | **Floating-point state QEMU can read**: rounding mode and accrued exception flags live in QEMU's `float_status`; they must be written back before any helper or control-register read can see them | `target/riscv/fpu_helper.c`, `target/arm/vfp_helper.c` | AOK runs guest FP on the host FPU (`emu/fpenv.c`); needs a sync at block boundaries. The most error-prone item on this list |
| 11 | **Decline when QEMU needs its own translation**: single-stepping, breakpoints, instruction counting, record/replay, plugins | `cflags` | New, trivial |
| 12 | **Thread safety**: one host thread per guest CPU, one shared cache, generation under QEMU's lock | `accel/tcg/tcg-accel-ops-mttcg.c` | AOK's generator is per-process; needs no shared mutable state of its own |

Reused from QEMU without change: every device, firmware and BIOS, disk
formats, networking, display, the monitor, snapshots, the whole of UTM's
front end and VM format, and kernel-mode CPU behaviour for every guest.

Size of what would be ported, for scale: AOK's riscv64 gadgets are about
16,300 lines of assembly and its arm64 gadgets about 12,100, plus the
per-guest decoders in `jit/gen.c`.

## 5. Offering it upstream

- **Target.** Build on `tctiSH/qemu` (QEMU 11.1), not on UTM's QEMU 10: it is
  the QEMU version UTM is moving to, the engine there is the one a fallback
  block should run on, and its author is actively taking engine work. TCTI is
  not in mainline QEMU, so mainline is not a target. To survive whichever
  back end UTM ends up with, AOK's generator should not emit TCTI's stream
  format: it should enter and leave its own blocks itself and depend only on
  QEMU's generic engine code (`accel/tcg`), which mainline maintains.
- **UTM's rules for AI-assisted contributions** (`CONTRIBUTING.md`): one
  well-defined feature or fix per pull request; a human must test the change
  before the PR is opened; no unrelated files touched; an
  `Assisted-by: AGENT:MODEL` tag rather than `Co-authored-by`; and their
  `/utm-review` and `/utm-submit` skills where the tool supports them.
- **Size.** "One well-defined feature" argues for a sequence: first the hook
  with a generator that declines everything, then one guest's integer core,
  then memory, branches, FP, vectors. Each is testable on its own because of
  the fallback.
- **Licence.** QEMU is GPLv2 (most files "or later"). AOK's riscv64 and arm64
  guest engines are entirely this fork's own commits, so they can be offered
  under whatever QEMU needs; `LICENSE.md` already names QEMU as a reason for
  the GPLv2 grant. Shared files with upstream iSH history need a per-file
  check before anything is copied.
- **Mainline QEMU** is a separate matter and not needed for this: TCTI itself
  is not there.

## 6. Risks and unknowns

- **Not yet measured on a device**, nor with AOK's gadgets actually running
  against QEMU's TLB and state layout. The projection assumes the cost per
  instruction carries over.
- **Kernel-heavy work stays slow relative to AOK today**, because the kernel
  is emulated in any full VM. This is a property of the product, not of the
  engine.
- **Floating-point state** (item 10) is where a subtle wrong-result bug would
  hide. QEMU's own test suites for the guest are the oracle.
- **Maintenance against QEMU.** The generator depends on internals
  (`TranslationBlock`, the TLB entry layout, helper signatures). A QEMU version
  bump in UTM means a rebase.
- **Binary size.** TCTI is already 320 MB per QEMU binary; AOK's gadgets add
  megabytes, not hundreds.
- **Whether UTM wants it.** Not asked yet.

## 7. Next steps

1. ~~Build option B for riscv64 on the tctiSH fork.~~ Done: section 8.
2. **Talk to the fork's author and UTM's maintainer.** There is now a result
   to show. Needs the branch published first.
3. **Measure on an iPad**: UTM SE from the App Store against AOK, same
   userland, to confirm the ratio on the hardware that matters.
4. Re-run this race when either engine changes: `tests/bench-utm/`.

## 8. The RISC-V plug-in

Built on `tctiSH/qemu` at `ded0cff` (QEMU 11.1.2), as a local branch
`aok-gadgets` in `~/git/qemu-aok`: five commits, 8 new files of about 2,900
lines, and 200 lines added to 13 existing QEMU files (4 lines changed).
Nothing has been published.
Its own documentation is `docs/devel/aok-gadgets.rst` in that tree.

It is option B of section 3, with one difference from section 4's sketch:
AOK's blocks do **not** use TCTI's stream format or entry point. They have
their own (`aok_tb_exec`), so the plug-in depends only on QEMU's generic
engine code and works whichever back end UTM ends up with.

### What it does

- Translates RV64 integer code (base, M, A, Zba, Zbb, Zbs, Zicond, C), loads
  and stores, branches and jumps, CSR accesses, and F and D floating point.
  Everything else ends the block and is QEMU's: `ecall`, `sret`, vectors.
- 99.8% of the blocks in a boot and workload run are AOK's.
- Double-precision arithmetic runs on the host's FP unit where that gives
  QEMU's software-float answer exactly, flags included, and calls software
  float where it might not.

### Results

Host CPU cycles on the M5's Super cores, RV64 guest, one CPU, lower is
better. "Plug-in" is AOK's gadgets inside QEMU; "tctiSH" is the same QEMU
binary with them switched off (`QEMU_AOK=0`); the three were run interleaved.

| Workload | AOK CLI | Plug-in | tctiSH fork | UTM SE today | Plug-in vs fork | Plug-in vs UTM SE today |
|---|--:|--:|--:|--:|--:|--:|
| CoreMark | 4.2 | 6.8 | 11.7 | 21.4 | 1.7x | 3.2x |
| n-body (floating point) | 3.1 | 3.4 | 10.1 | 22.5 | 2.9x | 6.5x |
| Python | 7.8 | 13.0 | 34.1 | 92.2 | 2.6x | 7.1x |
| xz | 4.4 | 5.8 | 9.7 | 21.7 | 1.7x | 3.8x |
| sha256sum | 2.8 | 3.4 | 5.2 | 9.6 | 1.5x | 2.9x |
| awk | 2.4 | 5.1 | 12.2 | 36.5 | 2.4x | 7.2x |
| Geometric mean | | | | | **2.1x** | **4.7x** |

A whole guest boot and shutdown, which is mostly kernel code: 11.6 G cycles
on the fork's engine, 6.0 G with the plug-in.

The plug-in is still about 1.5x behind AOK's own CLI (1.1x to 2.1x). What
separates them is mostly what AOK's generator does beyond one gadget per
instruction (fused pairs, cached register runs), which has not been carried
over, and for awk the emulated kernel.

### How it was checked

The reference is QEMU's own engine in the same binary. Three guest-side
tests, each compared between the two:

- **Program transcript** (`bench/check.sh`): CoreMark's checksums, Python
  big-integer and float arithmetic, xz and gzip, four hashes, sort, awk,
  shell loops, and a hash of every binary in the image. Identical with one
  guest CPU and with four.
- **Floating-point transcript** (`bench/fptest.c`): each double-precision
  operation over 67 awkward operands in all four rounding modes, hashing
  every result's bits with the exception flags it raised. Identical on all
  120 lines. Its first run found a real defect in the first version of the
  FP gadgets (6 lines differed): a product equal to the smallest normal can
  have been rounded up from a tiny value, and then RISC-V raises underflow
  and AArch64's flag cannot be used to tell.
- **Stress**: four transcripts at once on a four-CPU guest, all four equal
  to the reference.

### Bugs worth remembering

- **QEMU 11.1 stores its per-mode TLB tables in reverse order**
  (`tlb.f[mmuidx_to_fast_index()]`). Probing `tlb.f[mmu_idx]` found some
  other mode's table: every access missed and took the slow path. Correct
  output, four times slower than the engine being replaced.
- **`GETPC()` is a variable under TCTI.** A helper called from a gadget saw
  the position of the last helper call some *other* block had made, and when
  it raised an exception QEMU unwound the CPU to that unrelated block.
  OpenSBI probes for CSRs by trapping, so the firmware hung with the wrong
  hart id.
- Both of these produced a system that booted and gave right answers, or
  hung, rather than one that quietly computed wrong results.

### Not done

- Measured on a Mac only. An iPad run needs the plug-in built into a UTM
  build, which is the next real step.
- arm64 and x86 guests. Each needs its gadgets retargeted the same way.
- Single-precision FP natively, `ecall`/`sret` without leaving the stream,
  and AOK's fused gadgets.
- QEMU's own TCG test suites for RISC-V have not been run against it.

## Appendix: reproducing the race

The harness is in `tests/bench-utm/`, exactly as it ran. It expects to be
copied to `~/aok-vm-bench` on a Mac that has an AOK checkout with `build/ish`
and `build/tools/fakefsify`, plus Homebrew's `glib`, `pixman`, `meson` and
`ninja`. Order: `build-qemu.sh`, `setup-roots.sh`, `update-roots.sh`,
`cc -O2 -o cycwrap cycwrap.c`, `race.sh 3 > race.log`, then
`summarize.py race.log`.

| File | Purpose |
|---|---|
| `build-qemu.sh` | downloads UTM's QEMU 10.0.12 and builds it twice (TCTI, JIT) |
| `build-qemu-tctish.sh` | clones and builds the tctiSH fork (engine name `tctish` in `race.sh`) |
| `setup-roots.sh`, `update-roots.sh` | build the per-arch Alpine userland as an AOK root and as an initramfs |
| `bench/` | workload sources and the guest-side scripts |
| `cycwrap.c` | per-workload host cycles, instructions and Super-core share |
| `profile-tcti.sh` | samples UTM SE's engine during one workload |
| `race.sh` | the race: rounds x arch x engine, one process at a time |
| `summarize.py` | the tables above |
| `boot-aok.sh`, `run-aok.sh` | boot the RISC-V guest on the plug-in build (`~/aok-vm-bench/build-aok`), gadgets on or off |
| `bench/check.sh`, `bench/fptest.c` | the program and floating-point transcripts of section 8 |
| `prof-aok.sh` | where the plug-in build's time goes, by category |
