# Keeping ish-AOK-bind mergeable with upstream

This fork tracks [emkey1/ish-AOK](https://github.com/emkey1/ish-AOK) `working`,
which commits several times a day. `.github/workflows/sync-upstream.yml` merges
it daily and lands the merge only if it is clean and the tree still passes
`tools/check-fork-additions.sh`, the build, and both guest test suites. A
conflict stops the sync and opens an issue.

The October 2026 sync (1225 commits, after five weeks of the sync silently not
running) hit 23 conflicted files. Almost none of them were real disagreements.
They were this fork's lines sitting next to lines upstream had also changed.
This document is how the fork is laid out so that happens as rarely as
possible.

## The rule: our code in our files, one line where they meet

Git only conflicts where both sides changed the same or adjacent lines. So the
less of upstream's text this fork edits, the less there is to conflict.

- **New code goes in a new file.** A feature this fork adds gets its own `.c`.
  Upstream's file gets a call to it, one line marked `// ish-AOK-bind`.
- **One include, one table line, one call.** Where upstream's file has to
  reach ours, it does so through a single header include, a single table
  entry or macro, or a single function call, never a block.
- **Never append to the end of an upstream list.** Upstream appends there
  too, and two appends at the same spot always conflict. Put fork entries in a
  block of their own (as in `meson.build` and `tests/manual/setup-regressions.sh`),
  or next to an entry in the middle that upstream has no reason to touch (as
  `fs/aok.c` does with `aokfs_tools_android_dir` next to ktop).
- **Mark every hook `ish-AOK-bind`.** `grep -rn ish-AOK-bind` then lists every
  place this fork touches an upstream file.
- **Every hook is checked.** A merge that drops a hook still builds; the feature
  behind it just stops being called. `tools/check-fork-additions.sh` checks each
  hook is present, and the sync runs it.

## Where things live

| Fork file | What it is | Hooked from |
|---|---|---|
| `kernel/binder.c`, `fs/binderfs.c`, `kernel/ashmem.c`, `kernel/dma_heap.c`, `kernel/property_area.c`, `kernel/logd_sink.c`, `fs/selinuxfs.c` | the Android drivers | `fs/dev.c`, `fs/mount.c`, `app/AppDelegate.m`, and the fork block in `meson.build` |
| `kernel/arm64_fault.c` | arm64 page-fault handling: TBI resolve, the re-fault loop guard, the fault panel, `/proc/ish/arm64_faultdump` | `kernel/calls.c`: `fork_fault_untag`, `fork_fault_spinning`, `fork_arm64_fault_report`, `fork_arm64_fault_detail`, `fork_note_interrupt` |
| `emu/arm64_watch.c` | the arm64 store trace (`/proc/ish/arm64_watch`) | `emu/tlb.c`: `tlb_write_ptr_slow`'s `arm64_watch_record` call |
| `emu/tbi_probe.c` | names whoever hands the TLB a tagged address | `emu/tlb.c`: `FORK_TBI_NOTE_MISS` in `tlb_handle_miss` |
| `emu/fork_hooks.h` | declarations for the two above, and `fork_tbi_untag` | `emu/tlb.c`'s one include |
| `fs/proc/ish_fork.c`, `fs/proc/ish_fork.h` | this fork's `/proc/ish` entries | `fs/proc/ish.c`: `FORK_PROC_ISH_ENTRIES`, first line of the table |
| `fs/proc/pid_attr.c` | `/proc/<pid>/attr` (SELinux) | `fs/proc/pid.c`: the `"attr"` entry and `proc_pid_attr_init()` |

## What cannot be moved: TBI in the arm64 gadgets

AArch64 ignores the top byte of a pointer (TBI), and bionic's Scudo allocator
tags every heap pointer there. Upstream does not run Android and has **no TBI
handling at all**, so every arm64 memory path upstream adds or rewrites arrives
without it. A missing mask makes every access through an Android heap pointer
miss the TLB and fault.

These masks have to sit inside upstream's assembly, so they are the one part
of the fork that cannot be moved out. Each is a single
`and xN, xN, #0x00ffffffffffffff` marked `ish-AOK-bind`:

- `jit/guest-arm64/gadgets.h`: the top of `read_prep`/`write_prep`
- `jit/guest-arm64/atomics.S`: `a64_atomic_wptr` and `a64_atomic_rptr`, which
  every atomic calls; plus each exclusive's `x21`, since the monitor compares
  untagged addresses
- `jit/guest-arm64/simd.S`: `a64_xlate_r`, `a64_xlate_w`, `a64_vldst`, and
  `ld1st1_multi`'s inline probe
- `jit/guest-arm64/control.S`: `mops_tlb` and the MOPS dirty-page stores
- `jit/jit.c`: `arm64_ic_ivau`

`tools/check-fork-additions.sh` counts them per file.

**When a sync brings a new arm64 memory path** (anything that computes a TLB
index, calls `tlb_handle_miss`/`tlb_write_ptr_slow`, or adds `data_minus_addr`
to a guest address), it needs a mask before its first use of the address.
Leave the guest's own register tagged, because hardware does. Then add the
site to the count. `tests/manual/arm64/tagged_pointer.c` under
`tools/run-arm64-guest-tests.sh` is the test, and qemu-user is stricter than a
real core about this: a missed mask that a phone would forgive fails there.

## When the sync does conflict

1. `git fetch upstream working && git merge upstream/working` on `working`.
2. For each conflict, keep both sides' behaviour. If our side is a block in
   upstream's file, consider moving it out before resolving, so the next
   conflict cannot happen.
3. Run the gates the sync runs, in this order. `sh tools/check-fork-additions.sh`
   comes first, because it is the one that notices when our code has gone.
   Then `meson setup build -Dengine=jit && ninja -C build`,
   `meson test -C build float80 riscv64_decode e2e`,
   `bash tools/run-guest-tests.sh build`, and
   `bash tools/run-arm64-guest-tests.sh`.
4. Grep upstream's incoming changes to `jit/guest-arm64/` for new memory paths
   (above).
