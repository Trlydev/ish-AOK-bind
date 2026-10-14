// ish-AOK-bind: what this fork's one-line hooks in emu/tlb.c call.
//
// The code behind them lives in files upstream does not have (emu/arm64_watch.c,
// emu/tbi_probe.c), so a sync only ever has to merge around a single line
// where upstream's file and ours meet. See docs/fork-maintenance.md.
#ifndef EMU_FORK_HOOKS_H
#define EMU_FORK_HOOKS_H

#include "misc.h"
#include "kernel/abi.h"

struct tlb;

// AArch64 TBI: bits 56-63 of an arm64 guest pointer are a tag (bionic's Scudo
// sets them on heap pointers), not part of the address. For the C helpers the
// interpreter calls with a raw guest address; the JIT's gadgets mask in
// assembly.
static inline guest_addr_t fork_tbi_untag(guest_addr_t addr) {
    return (guest_addr_t) guest_abi_untag_addr(GUEST_ABI_ARM64, addr);
}

// The store trace (emu/arm64_watch.c). tlb_write_ptr_slow tests the state and
// records; everything else about the trace is in that file.
extern int arm64_watch_state; // 0 = unchecked, 1 = off, 2 = on
extern uint64_t arm64_watch_val; // ISH_ARM64_WATCH_VAL; the trace probe keys on it too
void arm64_watch_record(struct tlb *tlb, guest_addr_t addr, void *ptr);

// The TBI leak tripwire (emu/tbi_probe.c): a tagged address reaching the TLB
// miss path means some caller forgot to strip it, and this names the caller.
void tlb_note_tagged_miss(guest_addr_t addr, int type, void *from, void *ptr);
#define FORK_TBI_NOTE_MISS(addr, type, ptr) do { \
        if (unlikely(((addr) >> 56) != 0)) \
            tlb_note_tagged_miss((addr), (type), __builtin_return_address(0), (ptr)); \
    } while (0)

#endif
