#include "emu/cpu.h"
#include "emu/tlb.h"
#include "platform/platform.h"
#include "kernel/mm.h"
#include "emu/interrupt.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/abi.h"
#include "jit/jit.h"

void arm64_watch_scan_value(guest_addr_t addr, const void *value, unsigned size);

// The pair atomics below are the arm64 interpreter's (emu/arm64_interp.c);
// the JIT's atomics are gadgets (jit/guest-arm64/atomics.S).
//
// Misaligned atomics take an alignment fault on real hardware (they
// are architecturally required to be naturally aligned). Enforcing that
// here is also a host-memory-safety requirement, not just conformance:
// the helpers below resolve ONE host page and then run a host atomic of
// up to 16 bytes at the resolved pointer, so a misaligned guest atomic
// straddling a page boundary would read/write host memory beyond the
// page that was actually resolved. Natural alignment guarantees the
// access can never cross a page. INT_ALIGN: SIGBUS/BUS_ADRALN at
// cpu->segfault_addr, as Linux sends (INT_GPF, before it, was taken for a
// page fault and retried forever).
static int arm64_atomic_alignment_fault(struct cpu_state *cpu, guest_addr_t addr) {
    cpu->segfault_addr = addr;
    cpu->segfault_was_write = true;
    return INT_ALIGN;
}

// Host-atomic compare-and-swap PAIR (CASP/CASPA/CASPL/CASPAL). sz is the
// per-register width (4 for the W-pair form, 8 for the X-pair form); the
// register pair maps to guest memory little-endian as [addr] = lo (Rs/Rt)
// and [addr+sz] = hi (Rs+1/Rt+1), so the whole pair is one 2*sz-byte
// little-endian value. expected/desired/old_out are {lo, hi} arrays
// old_out always receives the observed memory value, zero-extended per
// half for the 32-bit form. Returns 0 / INT_PF / INT_ALIGN.
//
// The 64-bit pair uses a 16-byte __atomic_compare_exchange on an
// unsigned __int128 — on an ARMv8.0 baseline host clang lowers that to an
// LDXP/STXP loop (or an outline-atomics libcall), never a v8.1+ CASP
// instruction, keeping the v8.0-only gadget invariant (verified by
// disassembly; see the hostcaps note in jit/gen.c's crypto probing).
int arm64_casp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
               unsigned sz, const uint64_t expected[2], const uint64_t desired[2],
               uint64_t old_out[2], uint32_t *swapped) {
    // AArch64 TBI. This is the C-side counterpart of read_prep/write_prep
    // (jit/guest-arm64/gadgets.h), which mask the top byte before they touch
    // the TLB -- and it exists because these helpers deliberately BYPASS those
    // macros: the gadget hands the raw guest address straight to C so the
    // transfer can span pages via tlb_read/tlb_write. Nothing else on that
    // route ever strips the tag, so it has to happen here, on entry, before
    // the address reaches a TLB index or an alignment check.
    addr = (guest_addr_t) guest_abi_untag_addr(GUEST_ABI_ARM64, addr);
    // Alignment requirement is the TOTAL pair size (2*sz), per the ARM ARM
    // — which also guarantees the access never crosses a page, so the
    // single resolved host pointer below covers the whole pair.
    if (addr & (2 * sz - 1))
        return arm64_atomic_alignment_fault(cpu, addr);
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = true;
        return INT_PF;
    }
    if (sz == 4) {
        uint64_t exp = (uint32_t) expected[0] | ((uint64_t) (uint32_t) expected[1] << 32);
        uint64_t des = (uint32_t) desired[0] | ((uint64_t) (uint32_t) desired[1] << 32);
        bool ok = __atomic_compare_exchange_n((uint64_t *) ptr, &exp, des, false,
                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *swapped = ok ? 1 : 0;
        old_out[0] = (uint32_t) exp;
        old_out[1] = (uint32_t) (exp >> 32);
    } else {
        unsigned __int128 exp = ((unsigned __int128) expected[1] << 64) | expected[0];
        unsigned __int128 des = ((unsigned __int128) desired[1] << 64) | desired[0];
        bool ok = __atomic_compare_exchange_n((unsigned __int128 *) ptr, &exp, des, false,
                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *swapped = ok ? 1 : 0;
        old_out[0] = (uint64_t) exp;
        old_out[1] = (uint64_t) (exp >> 64);
    }
    return 0;
}

// LDXP (load exclusive pair). sz is the per-register width (4 for the
// W-pair form, 8 for the X-pair form); memory maps little-endian as
// [addr] = lo (Rt) and [addr+sz] = hi (Rt2), same layout as arm64_casp.
// Loads the pair, writes it to val_out (each half zero-extended for the
// 32-bit form), and arms the exclusive monitor (excl_addr/excl_val/
// excl_val_hi) with the address and observed pair. Returns 0 / INT_PF /
// INT_ALIGN (alignment is the TOTAL pair size, per the ARM ARM — which also
// guarantees the single resolved host pointer covers the whole pair).
//
// The 64-bit pair is read as two 8-byte atomic loads, NOT one 16-byte
// __atomic_load: clang lowers a 16-byte atomic load to an LDXP/STXP loop
// that STORES the value back, which host-faults on guest-read-only pages
// where a real LDXP is architecturally fine. A cross-half torn read under
// concurrent modification is caught downstream: STXP's 16-byte CAS
// compares against the armed (torn) pair, fails, and the guest's LL/SC
// loop retries — same value-based-monitor caveat as the STXR path.
int arm64_ldxp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
               unsigned sz, uint64_t val_out[2]) {
    // AArch64 TBI. This is the C-side counterpart of read_prep/write_prep
    // (jit/guest-arm64/gadgets.h), which mask the top byte before they touch
    // the TLB -- and it exists because these helpers deliberately BYPASS those
    // macros: the gadget hands the raw guest address straight to C so the
    // transfer can span pages via tlb_read/tlb_write. Nothing else on that
    // route ever strips the tag, so it has to happen here, on entry, before
    // the address reaches a TLB index or an alignment check.
    addr = (guest_addr_t) guest_abi_untag_addr(GUEST_ABI_ARM64, addr);
    if (addr & (2 * sz - 1))
        return arm64_atomic_alignment_fault(cpu, addr);
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = false;
        return INT_PF;
    }
    if (sz == 4) {
        uint64_t v = __atomic_load_n((uint64_t *) ptr, __ATOMIC_SEQ_CST);
        val_out[0] = (uint32_t) v;
        val_out[1] = (uint32_t) (v >> 32);
    } else {
        val_out[0] = __atomic_load_n((uint64_t *) ptr, __ATOMIC_SEQ_CST);
        val_out[1] = __atomic_load_n((uint64_t *) ptr + 1, __ATOMIC_SEQ_CST);
    }
    cpu->arm64_excl_addr = addr;
    cpu->arm64_excl_val = val_out[0];
    cpu->arm64_excl_val_hi = val_out[1];
    return 0;
}

// STXP (store exclusive pair): fails fast (status 1, monitor cleared)
// if the monitor isn't armed for this guest address; otherwise attempts
// a host-atomic pair CAS(addr: {excl_val, excl_val_hi} -> desired) via
// arm64_casp, so the whole check-and-store is one atomic operation — a
// won CAS is STXP success (status 0), a lost CAS (another agent changed
// memory since the LDXP) is failure and the guest LL/SC loop retries.
// The monitor clears on every completed STXP, success or not
// (architected), but is PRESERVED on a fault return so the restarted
// instruction can succeed once the fault (e.g. COW break) is resolved —
// same contract as the stxr gadget's fault path. Returns 0 / INT_PF /
// INT_ALIGN.
int arm64_stxp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
               unsigned sz, uint64_t desired_lo, uint64_t desired_hi,
               uint32_t *status_out) {
    // AArch64 TBI. This is the C-side counterpart of read_prep/write_prep
    // (jit/guest-arm64/gadgets.h), which mask the top byte before they touch
    // the TLB -- and it exists because these helpers deliberately BYPASS those
    // macros: the gadget hands the raw guest address straight to C so the
    // transfer can span pages via tlb_read/tlb_write. Nothing else on that
    // route ever strips the tag, so it has to happen here, on entry, before
    // the address reaches a TLB index or an alignment check.
    addr = (guest_addr_t) guest_abi_untag_addr(GUEST_ABI_ARM64, addr);
    if (cpu->arm64_excl_addr != addr) {
        cpu->arm64_excl_addr = UINT64_MAX;
        *status_out = 1;
        return 0;
    }
    uint64_t expected[2] = { cpu->arm64_excl_val, cpu->arm64_excl_val_hi };
    uint64_t desired[2] = { desired_lo, desired_hi };
    uint64_t old[2];
    uint32_t swapped;
    int err = arm64_casp(cpu, tlb, addr, sz, expected, desired, old, &swapped);
    if (err != 0)
        return err;
    cpu->arm64_excl_addr = UINT64_MAX;
    *status_out = swapped ? 0 : 1;
    return 0;
}

// ---------------------------------------------------------------------------
// x86 LOCK-prefixed read-modify-write, done with REAL host atomics.
//
// Every locked instruction an x86 guest executes has to be atomic against two
// different things, and until build 553 the amd64 path was wrong about both:
//
//   * other guest threads -- each of which runs on its own host pthread, so
//     "atomic" here means the host CPU must see one indivisible RMW; and
//   * the kernel's own read-modify-writes on guest memory (FUTEX_WAKE_OP is
//     the live example), which are plain C11 atomics on the host pointer.
//
// The amd64 interpreter used to serialise locked instructions on the global
// `atomic_l_lock` instead. That is slow (one global mutex for the whole
// emulator), it does not interlock with a host atomic at all, and several
// forms -- `lock add [mem], reg` and its OR/AND/XOR/SUB/ADC/SBB siblings --
// never took the lock in the first place, so they lost updates even against
// other guest threads (measured: 3876 of 200000 increments dropped with four
// threads on `lock addl %reg, (mem)`).
//
// The rule these helpers implement:
//
//   aligned  -> one host atomic on the resolved host pointer. A naturally
//               aligned access of 1/2/4/8/16 bytes cannot cross a page, so a
//               single resolved page always covers it.
//   unaligned-> x86_atomic_unaligned below. x86 makes a misaligned LOCK
//               atomic too, against every other access to those bytes, and
//               the i386 ABI makes the 64-bit case ordinary: it aligns
//               uint64_t struct fields to 4 bytes, and Python 3.14 does a
//               `lock cmpxchg8b` on one at startup.
//
// `fn` computes the new value from the observed old one and MUST BE PURE: on
// the aligned path it is re-run for every compare-exchange retry. Flags are
// the caller's job, computed once from the *old_out / *new_out this returns.
//
// Returns 0, or INT_PF with cpu->segfault_addr/was_write set.

typedef qword_t (*x86_atomic_fn)(qword_t old, void *ctx);

static int x86_atomic_fault(struct cpu_state *cpu, struct tlb *tlb) {
    cpu->segfault_addr = tlb->segfault_addr;
    cpu->segfault_was_write = true;
    return INT_PF;
}

// ---------------------------------------------------------------------
// Misaligned LOCK.
//
// This used to take the global atomic_l_lock around a read and a write. That
// serialised locked instructions against each other, and nothing else: a
// plain store from another thread could land between the read and the write
// and be lost. tests/manual/x86_unaligned_lock measured up to 60% of such
// stores lost, on both x86 engines. Exact instead, two ways:
//
//   * The operand lies inside one 16-byte-aligned block. A 16-byte host
//     compare-exchange on the block, changing only the operand's bytes, is a
//     real atomic: a store anywhere in the block between the read and the
//     exchange makes it fail and go round again. That holds against plain
//     stores, host atomics, and other processes mapping the same page.
//   * It straddles two blocks, and maybe two pages, which no host atomic
//     covers. Take this address space's lock as a writer instead. Guest code
//     only runs under the read side, and so does every syscall touching guest
//     memory, so while it is held nothing else in this process reads or
//     writes the operand. That is the emulator's split lock. Another process
//     sharing the page is not held off, which is the one thing this does not
//     cover.
//
//     The upgrade never lets go of the read side (read_to_write_lock_timed):
//     this runs inside the JIT, holding jetsam_lock, and a structural writer
//     that got in would wait for jetsam while this waited for it. When it
//     cannot be the last reader in time, or another thread is already
//     upgrading, it backs out and the instruction runs again.
//
// `step` sees the value the operand held and says what to store, if anything.
// It must be pure: on the block path it is re-run for every retry.
// ---------------------------------------------------------------------

typedef bool (*x86_atomic_step)(qword_t old, qword_t *neu, void *ctx);

static int x86_atomic_in_block(struct cpu_state *cpu, struct tlb *tlb,
        guest_addr_t addr, unsigned size_bytes, x86_atomic_step step, void *ctx,
        qword_t *old_out, qword_t *new_out) {
    // 16-byte aligned, so the block is on one page, and so is its host copy.
    unsigned __int128 *block = tlb_write_ptr_slow(tlb, addr & ~(guest_addr_t) 15);
    if (block == NULL)
        return x86_atomic_fault(cpu, tlb);
    unsigned shift = (addr & 15) * 8;
    unsigned __int128 ones = size_bytes == 8 ? (unsigned __int128) UINT64_MAX
        : ((unsigned __int128) 1 << (size_bytes * 8)) - 1;
    unsigned __int128 mask = ones << shift;
    // Starting from a guess is fine: a wrong guess fails the exchange, which
    // hands back what the block really holds.
    unsigned __int128 cur = 0, neu;
    qword_t old, val;
    bool store;
    do {
        old = (qword_t) ((cur & mask) >> shift);
        store = step(old, &val, ctx);
        // Storing nothing still exchanges the block with itself, so `old` is
        // a value it really held -- which is also what x86 does on a failed
        // CMPXCHG: it writes back what it read.
        neu = store ? (cur & ~mask) | (((unsigned __int128) val << shift) & mask) : cur;
    } while (!__atomic_compare_exchange_n(block, &cur, neu, false,
                 __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    *old_out = old;
    *new_out = store ? val : old;
    return 0;
}

// One thread at a time attempts the upgrade below. Two readers each waiting
// to be the last reader would wait for each other; the loser backs out
// instead of waiting here, because waiting while it holds the read lock would
// keep the winner from ever being alone.
static lock_t x86_split_lock = LOCK_INITIALIZER;

// Hand the instruction back to be run again. A page fault on an address that
// is mapped and writable resolves to nothing, so the engine leaves the JIT --
// releasing mem read and jetsam -- and re-executes the instruction.
static int x86_atomic_split_retry(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr) {
    tlb->segfault_addr = addr;
    return x86_atomic_fault(cpu, tlb);
}

// The split path's two halves. begin makes both pages present and writable
// and takes the lock: 0, with the operand's first out[2] bytes at out[0] and
// the rest at out[1]; or INT_PF with nothing held -- a fault, or a retry. end
// lets go. jit/gadgets-aarch64's x86_lock_rmw does the operation in between.
int x86_lock_split_begin(struct cpu_state *cpu, struct tlb *tlb,
        guest_addr_t addr, unsigned size_bytes, uintptr_t out[3]) {
    struct mem *mem = container_of(tlb->mmu, struct mem, mmu);
    guest_addr_t last = addr + size_bytes - 1;
    unsigned first = PAGE_SIZE - PGOFFSET(addr);
    if (first > size_bytes)
        first = size_bytes;
    // Make both pages present and writable -- breaking copy-on-write and
    // dropping code compiled from them -- while this thread holds only the
    // read side, which is all those paths may be called with.
    if (tlb_write_ptr_slow(tlb, addr) == NULL)
        return x86_atomic_fault(cpu, tlb);
    if (tlb_write_ptr_slow(tlb, last) == NULL) {
        // Linux reports the first byte that faulted -- the start of the
        // second page -- not the operand's last byte (camd: a `lock incl`
        // two bytes before a PROT_NONE page gives si_addr = that page).
        tlb->segfault_addr = last & ~(guest_addr_t) (PAGE_SIZE - 1);
        return x86_atomic_fault(cpu, tlb);
    }
    if (trylock(&x86_split_lock) != 0)
        return x86_atomic_split_retry(cpu, tlb, addr);
    // Siblings running guest code hold the read side until their next exit
    // from the JIT, which on its own can be a timer tick away: a poke sends
    // them out at the next block boundary, as the mmap barrier does
    // (kernel/mmap.c). The upgrade then holds them off until it is done.
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    uint64_t now = (uint64_t) now_ts.tv_sec * 1000000000ull + (uint64_t) now_ts.tv_nsec;
    // (A sibling that did one of these in the last 50ms is at it too.)
    const uint64_t recent = 50ull * 1000 * 1000;
    unsigned others = task_poke_shared_mem_counting(current, mem, now > recent ? now - recent : 1);
    if (current != NULL) {
        atomic_store_explicit(&current->split_lock_stamp, now, memory_order_relaxed);
        // This one is taking the address space from a sibling that was
        // running guest code and is not at this itself: afterwards this
        // thread stands aside for a quantum. It pokes itself, which takes it
        // out of the JIT at the next block boundary as a poke takes anyone,
        // and kernel/task.c's run loop does the waiting there, holding
        // nothing. Without it a thread doing these in a loop starves the
        // rest of its process: each needs the address space to itself, the
        // siblings are poked out of guest code for it, and the next begins
        // a few instructions after the last ended -- a sibling got a block
        // or two between them, and in 1.5 s a second thread had not
        // finished starting (tests/manual/x86_unaligned_lock.c, "a thread
        // never ran"). Only for such a sibling: threads that are all doing
        // split operations hand the lock among themselves as before, where
        // a quantum each turned seconds into minutes (docs/TODO.md).
        //
        // Not after every one: it has 200us of them between quanta. A
        // compare-and-swap loop (i386's 64-bit atomics are cmpxchg8b in one)
        // reads, tries, and on failure tries again with the value the
        // failure handed it; a quantum between the failure and the retry
        // gives the sibling time to change the value again, every time, and
        // the loop never gets through.
        if (others != 0 && now - current->split_lock_stood_aside >= 200000) {
            current->split_lock_wait = true;
            cpu_poke(&current->cpu);
        }
    }
    if (!read_to_write_lock_timed(&mem->lock, 20 * 1000 * 1000)) {
        unlock(&x86_split_lock);
        return x86_atomic_split_retry(cpu, tlb, addr);
    }
    // No other writer could get in while this thread kept its read lock, so
    // the pages faulted in above are still there. Look anyway: it is cheap,
    // and a NULL here must be a fault, not a wild pointer.
    char *lo = mem_ptr_locked(mem, addr, MEM_WRITE);
    char *hi = mem_ptr_locked(mem, last, MEM_WRITE);
    if (lo == NULL || hi == NULL) {
        write_to_read_lock(&mem->lock);
        unlock(&x86_split_lock);
        return x86_atomic_split_retry(cpu, tlb, lo == NULL ? addr : last);
    }
    // The part on the second page, if the operand reaches one.
    unsigned second = size_bytes - first;
    out[0] = (uintptr_t) lo;
    out[1] = second != 0 ? (uintptr_t) (hi - (second - 1)) : 0;
    out[2] = first;
    return 0;
}

void x86_lock_split_end(struct tlb *tlb) {
    struct mem *mem = container_of(tlb->mmu, struct mem, mmu);
    write_to_read_lock(&mem->lock);
    unlock(&x86_split_lock);
}

static int x86_atomic_split(struct cpu_state *cpu, struct tlb *tlb,
        guest_addr_t addr, unsigned size_bytes, x86_atomic_step step, void *ctx,
        qword_t *old_out, qword_t *new_out) {
    uintptr_t at[3];
    int err = x86_lock_split_begin(cpu, tlb, addr, size_bytes, at);
    if (err != 0)
        return err;
    char *lo = (char *) at[0], *hi_start = (char *) at[1];
    unsigned first = (unsigned) at[2], second = size_bytes - first;
    qword_t old = 0, val = 0;
    memcpy(&old, lo, first);
    if (second != 0)
        memcpy((char *) &old + first, hi_start, second);
    bool store = step(old, &val, ctx);
    if (store) {
        memcpy(lo, &val, first);
        if (second != 0)
            memcpy(hi_start, (char *) &val + first, second);
    }
    x86_lock_split_end(tlb);
    *old_out = old;
    *new_out = store ? val : old;
    return 0;
}

static int x86_atomic_unaligned(struct cpu_state *cpu, struct tlb *tlb,
        guest_addr_t addr, unsigned size_bytes, x86_atomic_step step, void *ctx,
        qword_t *old_out, qword_t *new_out) {
    if ((addr & 15) + size_bytes <= 16)
        return x86_atomic_in_block(cpu, tlb, addr, size_bytes, step, ctx, old_out, new_out);
    return x86_atomic_split(cpu, tlb, addr, size_bytes, step, ctx, old_out, new_out);
}

// An x86_atomic_fn as a step that always stores.
struct x86_atomic_fn_step {
    x86_atomic_fn fn;
    void *ctx;
};
static bool x86_atomic_fn_as_step(qword_t old, qword_t *neu, void *ctx) {
    struct x86_atomic_fn_step *s = ctx;
    *neu = s->fn(old, s->ctx);
    return true;
}

static int x86_atomic_rmw_unaligned(struct cpu_state *cpu, struct tlb *tlb,
        guest_addr_t addr, unsigned size_bytes, x86_atomic_fn fn, void *ctx,
        qword_t *old_out, qword_t *new_out) {
    struct x86_atomic_fn_step s = { fn, ctx };
    return x86_atomic_unaligned(cpu, tlb, addr, size_bytes, x86_atomic_fn_as_step, &s,
                                old_out, new_out);
}

int x86_atomic_rmw(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                   unsigned size_bytes, x86_atomic_fn fn, void *ctx,
                   qword_t *old_out, qword_t *new_out) {
    if (addr & (size_bytes - 1))
        return x86_atomic_rmw_unaligned(cpu, tlb, addr, size_bytes, fn, ctx,
                                        old_out, new_out);
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL)
        return x86_atomic_fault(cpu, tlb);

#define X86_RMW_AT(TYPE) do {                                            \
        TYPE *p = ptr;                                                   \
        TYPE old = __atomic_load_n(p, __ATOMIC_RELAXED), neu;            \
        do {                                                             \
            neu = (TYPE) fn((qword_t) old, ctx);                         \
        } while (!__atomic_compare_exchange_n(p, &old, neu, true,        \
                     __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));               \
        *old_out = (qword_t) old;                                        \
        *new_out = (qword_t) neu;                                        \
    } while (0)
    switch (size_bytes) {
        case 1: X86_RMW_AT(uint8_t); break;
        case 2: X86_RMW_AT(uint16_t); break;
        case 4: X86_RMW_AT(uint32_t); break;
        default: X86_RMW_AT(uint64_t); break;
    }
#undef X86_RMW_AT
    return 0;
}

struct x86_cas_step {
    qword_t expected, desired;
};
static bool x86_cas_as_step(qword_t old, qword_t *neu, void *ctx) {
    struct x86_cas_step *c = ctx;
    *neu = c->desired;
    return old == c->expected;
}

// LOCK CMPXCHG / CMPXCHG8B. Compares [addr] against `expected`; on equal
// stores `desired`. *old_out always receives the observed value -- which is
// what CMPXCHG loads into the accumulator when the compare fails, and what
// its flags are computed from either way.
int x86_atomic_cas(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                   unsigned size_bytes, qword_t expected, qword_t desired,
                   qword_t *old_out, bool *swapped) {
    if (addr & (size_bytes - 1)) {
        struct x86_cas_step c = { expected, desired };
        qword_t neu;
        int err = x86_atomic_unaligned(cpu, tlb, addr, size_bytes, x86_cas_as_step, &c,
                                       old_out, &neu);
        *swapped = err == 0 && *old_out == expected;
        return err;
    }
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL)
        return x86_atomic_fault(cpu, tlb);
#define X86_CAS_AT(TYPE) do {                                            \
        TYPE exp = (TYPE) expected;                                      \
        bool ok = __atomic_compare_exchange_n((TYPE *) ptr, &exp,        \
                     (TYPE) desired, false, __ATOMIC_SEQ_CST,            \
                     __ATOMIC_SEQ_CST);                                  \
        *swapped = ok;                                                   \
        *old_out = (qword_t) exp; /* CAS writes the observed value back */ \
    } while (0)
    switch (size_bytes) {
        case 1: X86_CAS_AT(uint8_t); break;
        case 2: X86_CAS_AT(uint16_t); break;
        case 4: X86_CAS_AT(uint32_t); break;
        default: X86_CAS_AT(uint64_t); break;
    }
#undef X86_CAS_AT
    return 0;
}

// LOCK CMPXCHG16B. The instruction already requires 16-byte alignment (the
// caller raises #GP otherwise), so there is no unaligned path -- and that
// alignment is also what guarantees the single resolved page covers all 16
// bytes. expected/desired/old_out are {low qword, high qword}.
int x86_atomic_cas16b(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                      const qword_t expected[2], const qword_t desired[2],
                      qword_t old_out[2], bool *swapped) {
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL)
        return x86_atomic_fault(cpu, tlb);
    unsigned __int128 exp = ((unsigned __int128) expected[1] << 64) | expected[0];
    unsigned __int128 des = ((unsigned __int128) desired[1] << 64) | desired[0];
    bool ok = __atomic_compare_exchange_n((unsigned __int128 *) ptr, &exp, des,
                 false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    *swapped = ok;
    old_out[0] = (qword_t) exp;
    old_out[1] = (qword_t) (exp >> 64);
    return 0;
}

// XCHG with a memory operand -- implicitly locked on x86, prefix or not.
// A direct host exchange rather than a compare-exchange loop, because this
// one is hot: it is the store half of every spinlock acquire.
static qword_t x86_atomic_swap_fn(qword_t old, void *ctx) {
    (void) old;
    return *(qword_t *) ctx;
}
int x86_atomic_xchg(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                    unsigned size_bytes, qword_t value, qword_t *old_out) {
    if (addr & (size_bytes - 1)) {
        qword_t neu;
        return x86_atomic_rmw_unaligned(cpu, tlb, addr, size_bytes,
                x86_atomic_swap_fn, &value, old_out, &neu);
    }
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL)
        return x86_atomic_fault(cpu, tlb);
    switch (size_bytes) {
        case 1: *old_out = __atomic_exchange_n((uint8_t *) ptr, (uint8_t) value, __ATOMIC_SEQ_CST); break;
        case 2: *old_out = __atomic_exchange_n((uint16_t *) ptr, (uint16_t) value, __ATOMIC_SEQ_CST); break;
        case 4: *old_out = __atomic_exchange_n((uint32_t *) ptr, (uint32_t) value, __ATOMIC_SEQ_CST); break;
        default: *old_out = __atomic_exchange_n((uint64_t *) ptr, (uint64_t) value, __ATOMIC_SEQ_CST); break;
    }
    return 0;
}

void tlb_refresh(struct tlb *tlb, struct mmu *mmu) {
    if (tlb->mmu == mmu &&
            tlb->mem_changes == atomic_load_explicit(&mmu->changes, memory_order_relaxed)) {
        return;
    }
    tlb->mmu = mmu;
    tlb->dirty_page = TLB_PAGE_EMPTY;
    tlb->mem_changes = atomic_load_explicit(&mmu->changes, memory_order_relaxed);
    tlb_flush(tlb);
}

void tlb_flush(struct tlb *tlb) {
    // Acquire: every entry installed from here on is valid under this value,
    // and whatever was published before the bump that produced it -- the JIT's
    // code-write tracking flags, see tlb_handle_miss -- must be seen by then.
    tlb->mem_changes = atomic_load_explicit(&tlb->mmu->changes, memory_order_acquire);
    for (unsigned i = 0; i < TLB_SIZE; i++)
        tlb->entries[i] = (struct tlb_entry) {.page = 1, .page_if_writable = 1};
    for (unsigned v = 0; v < TLB_VICTIMS; v++)
        tlb->victim[v] = (struct tlb_entry) {.page = 1, .page_if_writable = 1};
    tlb->exec_ok_page = 0;
}

// ---- a compile decodes one version of its page ------------------------------
//
// Decoders fetch a field at a time: opcode, then ModRM, then displacement. A
// store from another thread landing between two of those fetches gives a block
// built from half the old instruction and half the new one, which no CPU ever
// executes. HotSpot patches running code this way: making a method not entrant
// writes a jmp-to-self (EB FE EB FE) over the aligned first word of its entry,
// then the jump's fifth byte, then its first four bytes, relying on an x86
// fetch seeing an aligned word whole. A 32-bit JVM on the i386 guest died of
// it: the entry was `89 84 24 00 c0 ff ff`, a compile fetched the old 89 and
// then the new ModRM, decoded `mov ebx, ebx; dec ecx`, and ran the jump's
// displacement byte f1.
//
// So for a page that has been written to, jit.c takes a copy of the page
// before compiling from it, an aligned 64-bit word at a time -- single-copy
// atomic, so each aligned 4-byte store is in it whole or not at all, which is
// what that patching protocol assumes of the hardware -- and every fetch the
// compile makes from that page comes from the copy. The compile may see code
// that is about to change; that is what a CPU fetching just before the store
// sees, and the store is noted and drops the block (jit.c's SMC notes). What
// it can no longer see is a mixture. Only the first page is copied: reading
// ahead could materialize a page nothing else touches, and a block's tail on
// the next page is fetched live, as before.
static __thread struct {
    bool active;
    guest_addr_t base;
    uint64_t words[PAGE_SIZE / 8];
} fetch_snapshot;

void tlb_fetch_snapshot_begin(struct tlb *tlb, guest_addr_t ip) {
    fetch_snapshot.active = false;
    guest_addr_t base = ip & ~(guest_addr_t) (PAGE_SIZE - 1);
    const uint64_t *host = __tlb_read_ptr(tlb, base);
    if (host == NULL)
        return;
    for (unsigned i = 0; i < PAGE_SIZE / 8; i++)
        fetch_snapshot.words[i] = __atomic_load_n(&host[i], __ATOMIC_RELAXED);
    fetch_snapshot.base = base;
    fetch_snapshot.active = true;
}

void tlb_fetch_snapshot_end(void) {
    fetch_snapshot.active = false;
}

// A fetch that touches the snapshot page takes those bytes from the copy, and
// anything either side of it from memory.
static bool tlb_fetch_snapshotted(struct tlb *tlb, guest_addr_t addr, void *out, unsigned size) {
    guest_addr_t base = fetch_snapshot.base, end = base + PAGE_SIZE;
    if (addr >= end || addr + size <= base)
        return tlb_read(tlb, addr, out, size);
    uint8_t *o = out;
    if (addr < base) {
        unsigned n = (unsigned) (base - addr);
        if (!tlb_read(tlb, addr, o, n))
            return false;
        o += n;
        addr += n;
        size -= n;
    }
    unsigned n = (unsigned) (end - addr) < size ? (unsigned) (end - addr) : size;
    memcpy(o, (const uint8_t *) fetch_snapshot.words + (addr - base), n);
    if (n < size)
        return tlb_read(tlb, addr + n, o + n, size - n);
    return true;
}

bool tlb_fetch(struct tlb *tlb, guest_addr_t addr, void *out, unsigned size) {
    tlb->fetch_denied = false;
    if (size == 0)
        return true;
    uint64_t changes = atomic_load_explicit(&tlb->mmu->changes, memory_order_relaxed);
    page_t first = PAGE(addr), last = PAGE(addr + size - 1);
    for (page_t page = first; page <= last; page++) {
        if (tlb->exec_ok_page == page + 1 && tlb->exec_ok_changes == changes)
            continue;
        if (!mmu_page_executable(tlb->mmu, page)) {
            tlb->fetch_denied = true;
            tlb->segfault_addr = page == first ? addr : (guest_addr_t) page << PAGE_BITS;
            return false;
        }
        tlb->exec_ok_page = page + 1;
        tlb->exec_ok_changes = changes;
    }
    if (fetch_snapshot.active)
        return tlb_fetch_snapshotted(tlb, addr, out, size);
    return tlb_read(tlb, addr, out, size);
}

void tlb_free(struct tlb *tlb) {
    free(tlb);
}

bool __tlb_read_cross_page(struct tlb *tlb, guest_addr_t addr, char *value, unsigned size) {
    char *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return false;
    }
    char *ptr2 = __tlb_read_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL) {
        return false;
    }
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(value, ptr1, part1);
    memcpy(value + part1, ptr2, size - part1);
    return true;
}

bool __tlb_write_cross_page(struct tlb *tlb, guest_addr_t addr, const char *value, unsigned size) {
    char *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return false;
    }
    char *ptr2 = __tlb_write_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL) {
        return false;
    }
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(ptr1, value, part1);
    memcpy(ptr2, value + part1, size - part1);
    arm64_watch_scan_value(addr, value, size);
    return true;
}

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

static void tlb_note_tagged_miss(guest_addr_t addr, int type, void *from, void *ptr) {
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

// The page an entry is about to lose goes to the front of the victims, and
// those before `upto` move back one.
static inline void tlb_evict(struct tlb *tlb, const struct tlb_entry *evicted, unsigned upto) {
    struct tlb_entry keep = *evicted;
    for (unsigned v = upto; v > 0; v--)
        tlb->victim[v] = tlb->victim[v - 1];
    tlb->victim[0] = keep;
}

__no_instrument void *tlb_handle_miss(struct tlb *tlb, guest_addr_t addr, int type) {
    bool tagged = unlikely((addr >> 56) != 0);
    // The second chance (emu/tlb.h, TLB_VICTIMS): the page was evicted from
    // its entry by another that hashes there, and is still good. It changes
    // places with that one. Not when the table is stale, and not for a store
    // that has to be revalidated: those take the whole path below.
    struct tlb_entry *tlb_ent = &tlb->entries[TLB_INDEX(addr)];
    unsigned victim = TLB_VICTIMS;
    for (unsigned v = 0; v < TLB_VICTIMS; v++)
        if (tlb->victim[v].page == TLB_PAGE(addr))
            victim = v;
    if (victim != TLB_VICTIMS &&
            (type != MEM_WRITE || (tlb->victim[victim].page_if_writable == TLB_PAGE(addr) &&
                                   !tlb->mmu->requires_write_revalidate)) &&
            atomic_load_explicit(&tlb->mmu->changes, memory_order_relaxed) == tlb->mem_changes) {
        struct tlb_entry found = tlb->victim[victim];
        tlb_evict(tlb, tlb_ent, victim);
        *tlb_ent = found;
        if (type == MEM_WRITE)
            tlb->dirty_page = TLB_PAGE(addr);
        return (void *) (found.data_minus_addr + addr);
    }
    // SENSOR ONLY. A write miss is the closest thing AOK gets to a notification
    // that the guest is committing memory: the mmap guards approved the mapping
    // once, the pt entries were built then, and the host commits the physical
    // pages on touch with nothing else to see it.
    //
    // This must NEVER reclaim or wait. task_run_current holds
    // read_lock(&mem->lock) across the whole of cpu_run_to_interrupt
    // (kernel/task.c:907 to :918), so THIS THREAD already holds the read lock
    // here; swap_direct_reclaim takes the address-space barrier and wrlock_t is
    // neither recursive nor upgradable, so reclaiming here self-deadlocks. It
    // was measured doing exactly that: ten minutes at 0% CPU with this thread
    // in wrlock_acquire_locked and kswapd0 stuck behind it in mem_struct_lock.
    //
    // So all that happens here is a poke -- an atomic store, no lock -- which
    // the engines turn into INT_TIMER, landing in handle_timer_interrupt()
    // where task_run_current has already released the lock. That is the only
    // place in the emulator a guest thread holds nothing and can afford to wait.
    if (type == MEM_WRITE && current != NULL &&
            ++tlb->headroom_probe_countdown >= MEM_HEADROOM_PROBE_MISSES) {
        tlb->headroom_probe_countdown = 0;
        // The Mach trap in here is amortised over 4 MiB of touched pages, and
        // is only paid at all because the alternative is the app being killed.
        if (host_mem_should_reclaim()) {
            current->mem_throttle_wanted = true;
            cpu_poke(&current->cpu);
        }
    }
    char *ptr = mmu_translate(tlb->mmu, TLB_PAGE(addr), type);
    if (tagged)
        tlb_note_tagged_miss(addr, type, __builtin_return_address(0), ptr);
    if (atomic_load_explicit(&tlb->mmu->changes, memory_order_relaxed) != tlb->mem_changes)
        tlb_flush(tlb);
    if (ptr == NULL) {
        tlb->segfault_addr = addr;
        return NULL;
    }
    tlb->dirty_page = TLB_PAGE(addr);

    // (tlb_flush above may have emptied everything; the pointer stands.) A
    // victim's copy of this page that could not serve -- read-only where a
    // store came, or due revalidation -- goes, so the page is in one place;
    // and the page this one evicts joins the victims.
    if (victim != TLB_VICTIMS && tlb->victim[victim].page == TLB_PAGE(addr))
        tlb->victim[victim] = (struct tlb_entry) {.page = TLB_PAGE_EMPTY, .page_if_writable = TLB_PAGE_EMPTY};
    if (tlb_ent->page != TLB_PAGE(addr) && tlb_ent->page != TLB_PAGE_EMPTY)
        tlb_evict(tlb, tlb_ent, TLB_VICTIMS - 1);
    tlb_ent->page = TLB_PAGE(addr);
    if (type == MEM_WRITE)
        tlb_ent->page_if_writable = tlb_ent->page;
    else
        // 1 is not a valid page so this won't look like a hit
        tlb_ent->page_if_writable = TLB_PAGE_EMPTY;
    tlb_ent->data_minus_addr = (uintptr_t) ptr - TLB_PAGE(addr);
#if ENGINE_JIT
    // x86 code can be rewritten by a plain store. Once this entry is writable
    // no store to the page reaches mmu_translate again, so an executable page
    // -- or a shared one, which another mapping may execute -- is noted, after
    // the install (see jit_note_code_write), and the next compile from it
    // revokes the entry.
    //
    // jit_code_write_prepare sets the tracking flags and then bumps
    // mmu->changes. This entry is valid under tlb->mem_changes, which
    // tlb_flush read with acquire; if that was the bump or anything after it,
    // the flags read here are set, so no entry valid after the bump goes
    // unnoted.
    struct jit *jit = tlb->mmu->jit;
    if (type == MEM_WRITE && jit != NULL &&
            atomic_load_explicit(&jit->track_code_writes, memory_order_relaxed)) {
        unsigned flags = mmu_page_code_flags(tlb->mmu, PAGE(addr));
        if (((flags & P_EXEC) || ((flags & P_SHARED) &&
                atomic_load_explicit(&jit->track_shared_code, memory_order_relaxed))) &&
                !jit_note_code_write(jit, PAGE(addr), tlb))
            tlb_ent->page_if_writable = TLB_PAGE_EMPTY;
    }
#endif
    return (void *) (tlb_ent->data_minus_addr + addr);
}

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
static uint64_t arm64_watch_val;
static bool arm64_watch_val_on;
static int arm64_watch_state; // 0 = unchecked, 1 = off, 2 = on
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

static __no_instrument void arm64_watch_record(struct tlb *tlb, guest_addr_t addr, void *ptr) {
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
    // through the revalidate funnel), and every arm64 atomic that resolves via
    // tlb_write_ptr_slow -- the LSE read-modify-writes (LDADD/LDCLR/LDEOR/
    // LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN and SWP), CAS, CASP, STXR, and STXP.
    // STXR and STXP are easy to miss: neither resolves an address itself, they
    // reach arm64_cas / arm64_casp on a monitor hit and record from there.
    // Refcounts ARE visible: incStrong/decStrong are outline-atomics LDADD,
    // and they land here with the right address and the pre-store value.
    //
    // NOT recorded, all of which reach memory without passing this wrapper:
    // a store that straddles a page boundary (__tlb_write_cross_page), an
    // AdvSIMD ld1/st1 transfer (tlb_write), and LDXP -- which is a load.
    // Rule those out by the instruction's form before reading an absence as
    // "it never ran".
    //
    // A RECORD DOES NOT PROVE A STORE HAPPENED. arm64_cas and arm64_casp
    // resolve the pointer BEFORE they compare, so a failed CAS, a lost STXR
    // and a CAS-fail STXP each push a record with a correct address, a correct
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

// ISH_ARM64_TRACE_IP=<hex guest pc> + ISH_ARM64_TRACE_LDR="rn,rm": before
// the instruction at the traced pc executes, recompute its register-offset
// load address from the committed guest state and, when the value it is
// about to load matches ISH_ARM64_WATCH_VAL, dump the full context. Used to
// decide whether a poison value genuinely exists at the load's source or
// the executing gadget diverges from the architectural computation.
static uint64_t arm64_trace_rn = 5, arm64_trace_rm = 1;

uint64_t arm64_trace_ip_target(void) {
    static uint64_t target = (uint64_t) -1;
    if (target == (uint64_t) -1) {
        const char *spec = getenv("ISH_ARM64_TRACE_IP");
        target = (spec != NULL && spec[0] != '\0') ? strtoull(spec, NULL, 16) : 0;
        const char *regs = getenv("ISH_ARM64_TRACE_LDR");
        if (regs != NULL)
            sscanf(regs, "%llu,%llu",
                   (unsigned long long *) &arm64_trace_rn,
                   (unsigned long long *) &arm64_trace_rm);
    }
    return target;
}

void dump_mem(guest_addr_t start, uint_t len);

void arm64_trace_probe(struct cpu_state *cpu, struct tlb *tlb, uint64_t ip) {
    static _Atomic int hits;
    // ISH_ARM64_TRACE_LOOKUP=1 (paired with ISH_ARM64_TRACE_IP=<hex guest
    // pc>, see arm64_trace_ip_target above): call-site tracer for a specific
    // function whose calling convention is "(void *table, const char
    // *name, ...)" -- x1 is read as a NUL-terminated guest C string, x2/x3
    // and the return address (x30/LR) are logged alongside it. Written to
    // watch bfd_hash_lookup()/bfd_link_hash_lookup() calls (symbol name in
    // x1, create/copy flags in x2/x3) while diagnosing a GNU ld hang traced
    // to heap corruption in bfd's symbol hash table -- see
    // project_brk_reserve_shrink_regrow_corruption in the project's Claude
    // memory for the actual bug and fix (kernel/mmap.c, brk_reserve_start
    // shrink handling). General-purpose beyond that one investigation: any
    // "log every call to guest function at address X, with a C-string
    // second argument" question reuses this by pointing ISH_ARM64_TRACE_IP
    // at the new function's entry PC. Caps at 20000 logged calls so a hot
    // function traced by mistake doesn't flood the log.
    static int lookup_mode = -1;
    if (lookup_mode == -1)
        lookup_mode = getenv("ISH_ARM64_TRACE_LOOKUP") != NULL;
    if (lookup_mode) {
        static _Atomic int lhits;
        int n = atomic_fetch_add(&lhits, 1);
        if (n < 20000) {
            char namebuf[128];
            namebuf[0] = '\0';
            uint64_t strp = cpu->arm64_regs[1];
            for (int i = 0; i < 127; i++) {
                char c = 0;
                if (!tlb_read(tlb, strp + i, &c, 1))
                    break;
                namebuf[i] = c;
                namebuf[i + 1] = '\0';
                if (c == 0)
                    break;
            }
            printk("lookup#%d name=\"%s\" create=%llu copy=%llu caller=%llx\n", n,
                   namebuf,
                   (unsigned long long) cpu->arm64_regs[2],
                   (unsigned long long) cpu->arm64_regs[3],
                   (unsigned long long) cpu->arm64_regs[30]);
        }
        return;
    }
    uint64_t base = cpu->arm64_regs[arm64_trace_rn];
    uint64_t idx = cpu->arm64_regs[arm64_trace_rm];
    uint64_t addr = base + idx;
    uint64_t val = 0;
    if (!tlb_read(tlb, addr, &val, sizeof(val)))
        return;
    if (val != arm64_watch_val)
        return;
    if (atomic_fetch_add(&hits, 1) >= 100)
        return;
    printk("traceprobe ip=%#llx rn(x%llu)=%#llx rm(x%llu)=%#llx addr=%#llx val=%#llx\n",
           (unsigned long long) ip,
           (unsigned long long) arm64_trace_rn, (unsigned long long) base,
           (unsigned long long) arm64_trace_rm, (unsigned long long) idx,
           (unsigned long long) addr, (unsigned long long) val);
    for (int i = 0; i < 31; i += 4)
        printk("  x%d=%#llx x%d=%#llx x%d=%#llx x%d=%#llx\n",
               i, (unsigned long long) cpu->arm64_regs[i],
               i + 1, i + 1 < 31 ? (unsigned long long) cpu->arm64_regs[i + 1] : 0,
               i + 2, i + 2 < 31 ? (unsigned long long) cpu->arm64_regs[i + 2] : 0,
               i + 3, i + 3 < 31 ? (unsigned long long) cpu->arm64_regs[i + 3] : 0);
    printk("traceprobe mem around load addr %#llx:\n", (unsigned long long) addr);
    dump_mem((addr & ~7ULL) - 0x60, 0xc0);
    printk("traceprobe mem around x22=%#llx:\n", (unsigned long long) cpu->arm64_regs[22]);
    dump_mem((cpu->arm64_regs[22] & ~7ULL) - 0x20, 0x60);
    printk("traceprobe mem around dest x3=%#llx:\n", (unsigned long long) cpu->arm64_regs[3]);
    dump_mem((cpu->arm64_regs[3] & ~7ULL) - 0x40, 0x80);
}

__no_instrument void *tlb_write_ptr_slow(struct tlb *tlb, guest_addr_t addr) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (unlikely(arm64_watch_state == 2) && ptr != NULL)
        arm64_watch_record(tlb, addr, ptr);
    return ptr;
}
