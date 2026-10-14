// ish-AOK-bind: the arm64 store trace -- ISH_ARM64_WATCH_LO16/VAL from the
// environment, and /proc/ish/arm64_watch (off, all[:N][,global], lo16=, val=)
// on the device. Moved out of emu/tlb.c whole, upstream's original part with
// it, so that a sync merges around one call in tlb_write_ptr_slow instead of
// around this file's worth of changes. See docs/fork-maintenance.md.

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "emu/cpu.h"
#include "emu/tlb.h"
#include "emu/fork_hooks.h"
#include "kernel/task.h"

// ---- ISH_ARM64_WATCH_LO16 store watchpoint ------------------------------
// The V8-heap layout inside a page is stable across runs even though the
// allocation base moves, so the watch key is the low 16 bits of the target
// address. Every recorded entry keeps the storing instruction's guest pc
// (stashed in tlb->watch_ip by arm64_resolve_write_ptr), the full target
// address, and the 8 bytes about to be overwritten.

struct arm64_watch_rec {
    uint64_t ip;
    uint64_t addr;
    uint64_t oldval;
    uint32_t thread;
    // The guest tid, so a record can be matched against the task named on the
    // fault line ("ERROR: 2981(binder:2981_2) ..." prints the tid) and so the
    // dump can be scoped to the faulting task. pthread_self above identifies a
    // host thread and nothing a person reading dmesg can cross-check.
    uint32_t tid;
};
#define ARM64_WATCH_RING (1 << 20)
static struct arm64_watch_rec *arm64_watch_ring;
// 64-bit, because in "all" mode this counts EVERY store in every process that
// started after the trace was armed. A 32-bit counter wraps within a long
// reproduction, and a wrapped counter makes the dump's "how many records are
// really here" arithmetic silently wrong -- which is the arithmetic that
// decides whether an absent record means the store never happened.
static _Atomic uint64_t arm64_watch_idx;
static uint32_t arm64_watch_lo16;
static bool arm64_watch_lo16_on;
uint64_t arm64_watch_val; // also read by the trace probe in emu/tlb.c
static bool arm64_watch_val_on;
int arm64_watch_state; // 0 = unchecked, 1 = off, 2 = on (emu/fork_hooks.h)
// "record every store" mode. The lo16 and val filters above both key on
// something that does not survive a re-run -- an address low half, or a value
// -- and the question this was reached for ("did THIS instruction store, and to
// where") has no stable key at all under ASLR. So the usable filter is no
// filter: record every store into the ring and let the fault dump print the
// history leading up to the crash. Slow by construction (every store takes the
// write slow path) and meant to be switched on for one reproduction.
static bool arm64_watch_all;
// How many of the most recent records to print per dump. The ring holds 2^20;
// printing them all through printk on a phone is not a diagnostic, it is a
// hang, so the dump is bounded and the bound is settable with the mode.
static uint32_t arm64_watch_dump_limit = 64;
// Dump the last N records of the FAULTING task only (default), or the last N
// records globally (",global"). The ring is shared by every guest thread in
// every process -- one host process emulates all of them -- so a global window
// of N is not N stores of history for the task that crashed: a second process
// storing in the background pushes the interesting records out. Answering "did
// THIS instruction store" off a window that another process can shrink is how
// an absent record gets read as "the store never executed".
static bool arm64_watch_dump_global;

bool arm64_watch_enabled(void) {
    if (arm64_watch_state == 0) {
        const char *spec = getenv("ISH_ARM64_WATCH_LO16");
        if (spec != NULL && spec[0] != '\0') {
            arm64_watch_lo16 = (uint32_t) strtoul(spec, NULL, 16) & 0xffff;
            arm64_watch_lo16_on = true;
        }
        const char *vspec = getenv("ISH_ARM64_WATCH_VAL");
        if (vspec != NULL && vspec[0] != '\0') {
            arm64_watch_val = strtoull(vspec, NULL, 16);
            arm64_watch_val_on = true;
        }
        if (arm64_watch_lo16_on || arm64_watch_val_on || arm64_watch_all) {
            arm64_watch_ring = calloc(ARM64_WATCH_RING, sizeof(*arm64_watch_ring));
            arm64_watch_state = arm64_watch_ring != NULL ? 2 : 1;
        } else {
            arm64_watch_state = 1;
        }
    }
    return arm64_watch_state == 2;
}

static __no_instrument void arm64_watch_push(uint64_t ip, uint64_t addr, uint64_t val) {
    uint64_t i = atomic_fetch_add_explicit(&arm64_watch_idx, 1, memory_order_relaxed)
        & (ARM64_WATCH_RING - 1);
    struct arm64_watch_rec *rec = &arm64_watch_ring[i];
    rec->ip = ip;
    rec->addr = addr;
    rec->oldval = val;
    rec->thread = (uint32_t) (uintptr_t) pthread_self();
    rec->tid = current != NULL ? (uint32_t) current->pid : 0;
}

// The 8 bytes about to be overwritten, read without leaving the guest page.
// Guest pages are backed one host page at a time and the next one can be
// absent, so an unguarded 8-byte read of a store landing in the last bytes of
// a mapping's final page faults the EMULATOR -- taking the app down in the
// middle of the one reproduction the trace was armed for. The val filter has
// carried this guard since it was written (PGOFFSET <= PAGE_SIZE - 16); the
// "all" and lo16 paths did the same read without it.
static __no_instrument uint64_t arm64_watch_oldval(guest_addr_t addr, const void *ptr) {
    size_t avail = PAGE_SIZE - PGOFFSET(addr);
    if (avail > sizeof(uint64_t))
        avail = sizeof(uint64_t);
    uint64_t oldval = 0;
    memcpy(&oldval, ptr, avail);
    return oldval;
}

__no_instrument void arm64_watch_record(struct tlb *tlb, guest_addr_t addr, void *ptr) {
    if (arm64_watch_val_on) {
        // Read back the previous store resolved through this (per-thread)
        // tlb: if it deposited the poison value, record it with its ip.
        // Skip when a mapping change happened in between (the old host
        // page may be gone) or when reading 16 bytes would leave the
        // guest page the store was on.
        if (tlb->prev_write_ptr != NULL &&
                tlb->prev_write_changes == tlb->mem_changes &&
                PGOFFSET(tlb->prev_write_addr) <= PAGE_SIZE - 16) {
            uint64_t half[2];
            memcpy(half, tlb->prev_write_ptr, sizeof(half));
            if (half[0] == arm64_watch_val)
                arm64_watch_push(tlb->prev_write_ip, tlb->prev_write_addr, half[0]);
            else if (half[1] == arm64_watch_val)
                arm64_watch_push(tlb->prev_write_ip, tlb->prev_write_addr + 8, half[1]);
        }
        tlb->prev_write_ptr = (char *) ptr - (addr & 7);
        tlb->prev_write_addr = addr & ~(guest_addr_t) 7;
        tlb->prev_write_ip = tlb->watch_ip;
        tlb->prev_write_changes = tlb->mem_changes;
    }
    if (arm64_watch_all)
        arm64_watch_push(tlb->watch_ip, addr, arm64_watch_oldval(addr, ptr));
    if (arm64_watch_lo16_on) {
        // window: any store starting up to 15 bytes before the watched
        // 8-byte slot through its end can touch it (largest guest store
        // is 16 bytes)
        uint32_t lo = addr & 0xffff;
        if (lo + 16 > arm64_watch_lo16 && lo < arm64_watch_lo16 + 8)
            arm64_watch_push(tlb->watch_ip, addr, arm64_watch_oldval(addr, ptr));
    }
}

// Direct value check for bulk stores that see the data (the crosspage
// flush, and a64_vldst's stores while a watch is on): record any poison
// value in the buffer.
__no_instrument void arm64_watch_scan_value(guest_addr_t addr, const void *value, unsigned size) {
    if (arm64_watch_state != 2 || !arm64_watch_val_on)
        return;
    for (unsigned off = 0; off + 8 <= size; off += 4) {
        uint64_t v;
        memcpy(&v, (const char *) value + off, sizeof(v));
        if (v == arm64_watch_val)
            arm64_watch_push((uint64_t) -1, addr + off, v);
    }
}

// /proc/ish/arm64_watch. Accepts:
//   off                  disable
//   all[:N]              record EVERY store; dump the last N (default 64) of
//                        the FAULTING task
//   all[:N],global       ... of every task, sharing one window (the old
//                        behaviour; useful only when the question is about
//                        interleaving between tasks)
//   lo16=XXXX            the existing address-low-half filter
//   val=XXXXXXXX         the existing stored-value filter
// Configuring here rather than only from ISH_ARM64_WATCH_LO16 is the whole
// point: getenv on iOS reads the APP's environment, so the watchpoint was a
// development-machine feature and unusable on the one machine where the faults
// being chased actually happen -- exactly as /proc/ish/arm64_faultdump was.
//
// Set it BEFORE launching the process to be measured: arm64_watch_enabled()
// decides requires_write_revalidate at mem_init, so a process already running
// will not start funnelling its stores through here.
void arm64_watch_configure(const char *spec) {
    arm64_watch_lo16_on = false;
    arm64_watch_val_on = false;
    arm64_watch_all = false;
    arm64_watch_dump_limit = 64;
    arm64_watch_dump_global = spec != NULL && strstr(spec, ",global") != NULL;
    if (spec != NULL && strncmp(spec, "all", 3) == 0) {
        arm64_watch_all = true;
        if (spec[3] == ':')
            arm64_watch_dump_limit = (uint32_t) strtoul(spec + 4, NULL, 10);
        if (arm64_watch_dump_limit == 0 || arm64_watch_dump_limit > 4096)
            arm64_watch_dump_limit = 64;
    } else if (spec != NULL && strncmp(spec, "lo16=", 5) == 0) {
        arm64_watch_lo16 = (uint32_t) strtoul(spec + 5, NULL, 16) & 0xffff;
        arm64_watch_lo16_on = true;
    } else if (spec != NULL && strncmp(spec, "val=", 4) == 0) {
        arm64_watch_val = strtoull(spec + 4, NULL, 16);
        arm64_watch_val_on = true;
    }
    if (arm64_watch_lo16_on || arm64_watch_val_on || arm64_watch_all) {
        if (arm64_watch_ring == NULL)
            arm64_watch_ring = calloc(ARM64_WATCH_RING, sizeof(*arm64_watch_ring));
        arm64_watch_state = arm64_watch_ring != NULL ? 2 : 1;
    } else {
        arm64_watch_state = 1;
    }
}

void arm64_watch_show(char *out, size_t size) {
    if (arm64_watch_state != 2)
        snprintf(out, size, "off");
    else if (arm64_watch_all)
        snprintf(out, size, "all:%u%s", arm64_watch_dump_limit,
                 arm64_watch_dump_global ? ",global" : "");
    else if (arm64_watch_lo16_on)
        snprintf(out, size, "lo16=%04x", arm64_watch_lo16);
    else
        snprintf(out, size, "val=%llx", (unsigned long long) arm64_watch_val);
}

// Called from the fatal-fault path, on the faulting thread, so current->pid
// here is the task named on the ERROR: line.
//
// Scoped to that task by default. The question this trace exists to answer is
// "did instruction X store, and to what address", and the answer is read off
// the ABSENCE of a record as much as its presence -- so the window has to be N
// stores of THIS task's history, not N stores of whatever the machine happened
// to be doing. It is one ring for every guest thread in every process, and the
// reproduction it was built for needs a second process (servicemanager) running
// alongside the one that crashes.
//
// The trailing line is the other half of that: a window that ended because the
// limit was reached does not license "the store never executed", because older
// stores by the same task are still in the ring. Only a window that ran out of
// history does.
void arm64_watch_dump(void) {
    if (arm64_watch_state != 2)
        return;
    uint64_t end = atomic_load_explicit(&arm64_watch_idx, memory_order_relaxed);
    uint64_t held = end < ARM64_WATCH_RING ? end : ARM64_WATCH_RING;
    uint32_t tid = current != NULL ? (uint32_t) current->pid : 0;
    bool scoped = !arm64_watch_dump_global && tid != 0;

    // Walk back until the limit is filled or this task's history runs out,
    // then print forward from there. Two passes rather than one so the records
    // come out oldest-first without buffering up to 4096 indices.
    uint64_t scanned = 0, matched = 0;
    while (scanned < held && matched < arm64_watch_dump_limit) {
        struct arm64_watch_rec *rec =
            &arm64_watch_ring[(end - scanned - 1) & (ARM64_WATCH_RING - 1)];
        scanned++;
        if (!scoped || rec->tid == tid)
            matched++;
    }
    bool ran_out = scanned == held;

    if (scoped)
        printk("arm64 watch ring: %llu records total, %llu held; showing the last "
               "%llu of tid %u (scanned back %llu)\n",
               (unsigned long long) end, (unsigned long long) held,
               (unsigned long long) matched, tid, (unsigned long long) scanned);
    else
        printk("arm64 watch ring: %llu records total, %llu held; showing the last "
               "%llu (all tasks)\n",
               (unsigned long long) end, (unsigned long long) held,
               (unsigned long long) matched);

    uint64_t n = matched;
    for (uint64_t back = scanned; back > 0; back--) {
        struct arm64_watch_rec *rec =
            &arm64_watch_ring[(end - back) & (ARM64_WATCH_RING - 1)];
        if (scoped && rec->tid != tid)
            continue;
        printk("  watch[-%llu] ip=%#llx addr=%#llx old=%#llx tid=%u thread=%#x\n",
               (unsigned long long) n, (unsigned long long) rec->ip,
               (unsigned long long) rec->addr, (unsigned long long) rec->oldval,
               rec->tid, rec->thread);
        n--;
    }

    if (ran_out)
        printk("arm64 watch ring: window reaches the oldest record held, so a store "
               "missing above is missing from the whole trace\n");
    else
        printk("arm64 watch ring: window ended at the dump limit (%u), NOT at the "
               "start of history -- older stores are in the ring; a store missing "
               "above may simply be further back. Raise N in all:N.\n",
               arm64_watch_dump_limit);
    // What is and is not in here, because an absence is read as evidence.
    // This comment has now been wrong twice, in both directions, and each
    // version cost a round -- so it is written from what actually calls
    // tlb_write_ptr_slow rather than from what looks like it should.
    //
    // RECORDED: ordinary stores (the JIT's write path, which "all" forces
    // through the revalidate funnel), and every arm64 atomic: since upstream's
    // 5680ff7 they are inline gadgets, and each resolves its address through
    // a64_atomic_wptr, which takes tlb_write_ptr_slow whenever
    // requires_write_revalidate is set -- and arming the trace sets it. That
    // covers the LSE read-modify-writes (LDADD/LDCLR/LDEOR/LDSET/LDSMAX/
    // LDSMIN/LDUMAX/LDUMIN and SWP), CAS, CASP, STXR and STXP. Refcounts ARE
    // visible: incStrong/decStrong are outline-atomics LDADD. LD1-4/ST1-4
    // stores record too (a64_xlate_w takes the same slow path).
    //
    // NOT recorded: a store that straddles a page boundary
    // (__tlb_write_cross_page), and LDXP, which is a load. Rule those out by
    // the instruction's form before reading an absence as "it never ran".
    //
    // A RECORD DOES NOT PROVE A STORE HAPPENED. The atomics resolve the
    // pointer BEFORE they compare, so a failed CAS, a lost STXR and a
    // CAS-fail STXP each push a record with a correct address, a correct
    // pre-value and a correct ip for a store that never landed. Read the
    // old= value against what the instruction would have written before
    // concluding it wrote.
    //
    // The ip is only trustworthy for the arm64 JIT. The x86 LOCK helpers
    // (x86_atomic_rmw and friends) also record, and no i386/amd64 gadget sets
    // watch_ip, so their records carry 0 or a stale arm64 ip; the arm64
    // INTERPRETER is the same, since only the JIT gadgets stash it. The dump
    // is tid-scoped by default, which mostly contains both.
}
