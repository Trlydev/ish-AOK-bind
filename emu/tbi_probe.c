// ish-AOK-bind: the TBI leak tripwire. tlb_handle_miss calls
// tlb_note_tagged_miss (through FORK_TBI_NOTE_MISS) for any address that still
// carries an AArch64 tag; this names the caller. Kept out of emu/tlb.c so a
// sync merges around one line. See docs/fork-maintenance.md.

#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "emu/cpu.h"
#include "emu/tlb.h"
#include "emu/fork_hooks.h"
#include "kernel/task.h"
#include "emu/memory.h"

// AArch64 TBI leak probe. Every arm64-guest path that can reach this function
// is supposed to have stripped the top byte already -- the JIT's
// read_prep/write_prep (jit/guest-arm64/gadgets.h) mask before they compute
// the TLB index, the interpreter masks in arm64_mem_read/write, and the
// syscall helpers mask in kernel/user.c. So a tagged address arriving HERE
// says the leak is below the kernel, in whichever caller is one frame up,
// and a tagged address that reaches handle_page_fault_interrupt WITHOUT this
// firing says the opposite: the TLB layer was fine and cpu->segfault_addr
// picked the tag up somewhere else.
//
// That one bit is the difference between searching the gadget set and
// searching the fault plumbing, which is worth a branch on a path that
// already calls mmu_translate.
//
// The caller is named, not just printed. A bare return address is useless on
// the device this reproduces on: the image is position-independent, there is
// no symbol table to hand, and nobody is going to subtract a load address off
// a phone screen. The arm64 gadget helpers that can reach here are a handful
// of hand-written stubs of a dozen instructions each (memory.S), so comparing
// the return address against their entry points identifies the path exactly,
// and the answer arrives already in words.
#if defined(__aarch64__) && defined(ISH_JIT_ARM64_GUEST)
// Explicit asm labels, and they are load-bearing on Apple platforms. A C
// symbol is emitted with a leading underscore there, so a plain `extern void
// arm64_handle_read_miss(void)` asks the linker for _arm64_handle_read_miss --
// which does not exist, because memory.S declares these with BARE names (only
// assembly calls them, so they never needed the NAME() macro that the gadget
// symbols use for exactly this reason). Caught by the iOS build failing to
// link; the ELF cross build is happy either way, since there is no underscore
// to add. Naming them here rather than rewriting memory.S keeps the change out
// of hot hand-written assembly.
extern void arm64_handle_read_miss(void) __asm__("arm64_handle_read_miss");
extern void arm64_handle_write_miss(void) __asm__("arm64_handle_write_miss");
extern void arm64_resolve_write_ptr(void) __asm__("arm64_resolve_write_ptr");
extern void arm64_crosspage_load(void) __asm__("arm64_crosspage_load");
extern void arm64_crosspage_store(void) __asm__("arm64_crosspage_store");

// The device answered "not one of the arm64 gadget helpers", which was useful
// (it ruled out the whole assembly miss path) and then immediately became the
// wrong question: the caller is C, and the gadget stubs were the only names on
// the list. So the C functions that can reach tlb_handle_miss are anchors too.
//
// Nearest-below with the delta printed rather than a span cap: a C function is
// not a twelve-instruction stub, and guessing its length would just turn a
// real answer back into "unknown". A small delta names the caller; a delta in
// the tens of kilobytes says the caller is in some other translation unit
// entirely, which is itself worth knowing and cannot be mistaken for a match.
extern void *tlb_write_ptr_slow(struct tlb *tlb, guest_addr_t addr);
// Anchors from OTHER translation units. The device answered
// "tlb_handle_miss+0x873b0" -- 553KB past the nearest anchor, i.e. every name
// on the list was in this file and the caller is not. Under LTO the final
// layout bears no relation to any local build, so the only way to narrow it is
// to spread the anchors across the image.
extern void *mem_ptr(struct mem *mem, guest_addr_t addr, int type);
extern void tlb_flush(struct tlb *tlb);

struct tlb_caller_anchor { const void *addr; const char *name; };

static const char *tlb_arm64_caller_name(void *from, uintptr_t *delta_out) {
    const struct tlb_caller_anchor anchors[] = {
        // assembly stubs (jit/guest-arm64/memory.S)
        {(const void *) arm64_handle_read_miss,  "arm64_handle_read_miss"},
        {(const void *) arm64_handle_write_miss, "arm64_handle_write_miss"},
        {(const void *) arm64_resolve_write_ptr, "arm64_resolve_write_ptr"},
        {(const void *) arm64_crosspage_load,    "arm64_crosspage_load"},
        {(const void *) arm64_crosspage_store,   "arm64_crosspage_store"},
        // C, this file -- __tlb_write_ptr/__tlb_read_ptr are forceinline, so a
        // return address lands in whichever of these inlined them.
        {(const void *) tlb_write_ptr_slow,      "tlb_write_ptr_slow"},
        {(const void *) __tlb_read_cross_page,   "__tlb_read_cross_page"},
        {(const void *) __tlb_write_cross_page,  "__tlb_write_cross_page"},
        {(const void *) tlb_handle_miss,         "tlb_handle_miss (recursed?)"},
        {(const void *) tlb_flush,               "tlb_flush (emu/tlb.c)"},
        // other TUs
        {(const void *) mem_ptr,                 "mem_ptr (emu/memory.c)"},
    };
    const char *best = NULL;
    uintptr_t best_delta = (uintptr_t) -1;
    uintptr_t ret = (uintptr_t) from;
    for (unsigned i = 0; i < sizeof(anchors) / sizeof(anchors[0]); i++) {
        uintptr_t entry = (uintptr_t) anchors[i].addr;
        if (ret < entry)
            continue;
        if (ret - entry < best_delta) {
            best_delta = ret - entry;
            best = anchors[i].name;
        }
    }
    if (delta_out != NULL)
        *delta_out = best != NULL ? best_delta : 0;
    return best != NULL ? best : "below every known anchor";
}
#else
static const char *tlb_arm64_caller_name(void *from, uintptr_t *delta_out) {
    (void) from;
    if (delta_out != NULL)
        *delta_out = 0;
    return "n/a on this host";
}
#endif

void tlb_note_tagged_miss(guest_addr_t addr, int type, void *from, void *ptr) {
    // Budget is per emulator lifetime, and that is the point: a tagged address
    // reaching here should never happen at all, so eight of them is plenty to
    // name the culprit and few enough that nothing else in the log is pushed
    // out. Its short-lived companion -- a probe that logged every FAILING miss,
    // tagged or not -- had to be removed for exactly that reason: ordinary
    // demand-paging failures spent the whole budget during boot, and a later
    // reading of "zero failing misses" was then taken as evidence the miss had
    // SUCCEEDED. It had not; the log had simply stopped talking.
    enum { TAGGED_MISS_LOG_BUDGET = 8 };
    static unsigned tagged_miss_log_count;
    if (tagged_miss_log_count >= TAGGED_MISS_LOG_BUDGET)
        return;
    tagged_miss_log_count++;
    uintptr_t delta = 0;
    const char *name = tlb_arm64_caller_name(from, &delta);
    // ptr is the whole point of the second round. The device reported eight of
    // these and ZERO failing misses, which cannot both be true if the tagged
    // page is unmapped: PAGE(0x0200'7fff'bdff'd000) is far past any page_limit
    // this guest can have, so mem_pt_raw should reject it and the miss should
    // fail. Printing the translate result settles which half of that is wrong
    // instead of reasoning about it a third time.
    printk("arm64: TLB miss on TAGGED address %#llx (%s) -> translate %s (%p), "
           "called from %p = %s+%#lx\n",
           (unsigned long long) addr, type == MEM_WRITE ? "write" : "read",
           ptr != NULL ? "SUCCEEDED" : "failed", ptr,
           from, name, (unsigned long) delta);
}
