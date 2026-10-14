// ish-AOK-bind: this fork's arm64 page-fault handling and diagnostics, kept
// out of kernel/calls.c -- upstream's busiest file -- so that a sync merges
// around a few one-line calls instead of a few hundred lines threaded through
// handle_page_fault_interrupt. See docs/fork-maintenance.md.
//
//   fork_fault_untag        resolve against the untagged (TBI) address
//   fork_fault_spinning     the guard against a fault that resolves and
//                           re-faults forever
//   fork_arm64_fault_report the TAGGED POINTER verdict and the page panel
//   fork_arm64_fault_detail the arm64 registers in the fault record
//   arm64_faultdump_*       /proc/ish/arm64_faultdump
//   fork_note_interrupt     the per-thread syscall count the guard keys on

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "emu/cpu.h"
#include "emu/memory.h"
#include "kernel/abi.h"
#include "kernel/calls.h"
#include "kernel/task.h"
#include "emu/interrupt.h"
#include "util/sync.h"

// Which registers to dump guest memory around on an arm64 page fault: a comma
// separated list of register numbers, "1,3" for x1 and x3. Empty = off.
//
// This used to be readable ONLY from ISH_ARM64_FAULT_MEMDUMP in the
// environment, which quietly made it a development-machine feature: on iOS
// getenv returns the APP's environment, and nothing a guest shell exports can
// reach it. So the one diagnostic that could show a faulting object's memory
// was unavailable on the only machine where the interesting faults happen.
// /proc/ish/arm64_faultdump is the same setting by the route every other knob
// here uses; the environment variable still works and seeds the default, so
// existing notes and scripts are unaffected.
static lock_t arm64_faultdump_lock = LOCK_INITIALIZER;
static char arm64_faultdump_spec[64];
static bool arm64_faultdump_spec_valid;

void arm64_faultdump_get(char *out, size_t size) {
    lock(&arm64_faultdump_lock, 0);
    if (!arm64_faultdump_spec_valid) {
        const char *env = getenv("ISH_ARM64_FAULT_MEMDUMP");
        strlcpy(arm64_faultdump_spec, env != NULL ? env : "", sizeof(arm64_faultdump_spec));
        arm64_faultdump_spec_valid = true;
    }
    strlcpy(out, arm64_faultdump_spec, size);
    unlock(&arm64_faultdump_lock);
}

void arm64_faultdump_set(const char *spec) {
    lock(&arm64_faultdump_lock, 0);
    strlcpy(arm64_faultdump_spec, spec != NULL ? spec : "", sizeof(arm64_faultdump_spec));
    arm64_faultdump_spec_valid = true;
    unlock(&arm64_faultdump_lock);
}

void arm64_faultdump_show(char *out, size_t size) {
    arm64_faultdump_get(out, size);
}

// One shot at everything, instead of one hypothesis per device round.
//
// Six theories about the re-faulting arm64 store have now been killed one at a
// time -- the store form, copy-on-write, the fork window, the blocking-wait
// path, the gadget's change-counter check, and host page mirroring -- at
// roughly a device round each. That is the expensive way to do it. This prints
// the whole differential at the moment of failure so a single reproduction
// eliminates classes rather than one guess.
//
// What each line is for:
//
//   cfg          real_page_size vs PAGE_SIZE decides whether host page
//                mirroring is even compiled in as live (it is gated on their
//                being equal), so this settles ON THE DEVICE what was
//                previously settled by reading the source. requires_write_
//                revalidate and the changes counter say whether the JIT's
//                write slow path and TLB flush are armed at all.
//   retry        printed EVERY time round the loop, not once. If the flags
//                change between retries this is a race; if they are identical
//                sixteen times it is a logic bug that no amount of timing work
//                will find. Nothing else distinguishes those two, and they
//                call for completely different searches.
//   resolved     what mem_ptr_fault handed back. The log's claim is that the
//                page "is there"; this is the pointer that claim rests on.
//   host page    every guest page sharing the faulting address's HOST page.
//                On a 16 KB host that is four guest pages, and a protection
//                change to one is a protection change to all four -- so a
//                neighbour with different flags is a lead, and identical
//                flags across all four rules the aliasing idea out.
//   prot cache   whether the per-host-page protection cache exists for this
//                mapping, since a stale entry there is a documented previous
//                cause of exactly this symptom (emu/memory.c's comment about
//                faulting forever on a page the page tables call writable).
static void dump_arm64_fault_panel(guest_addr_t addr, unsigned retry, void *resolved) {
    enum { PANEL_BUDGET = 40 }; // ~2 loops' worth; enough to see evolution
    static unsigned panel_count;
    if (panel_count >= PANEL_BUDGET)
        return;
    panel_count++;

    struct mem *mem = current != NULL ? current->mem : NULL;
    if (mem == NULL)
        return;

    if (retry == 0)
        printk("arm64 panel cfg: real_page_size=%zu guest PAGE_SIZE=%d "
               "mirroring_live=%d write_revalidate=%d changes=%llu\n",
               real_page_size, (int) PAGE_SIZE,
               (int) mem_host_page_mirroring_available(),
               (int) mem->mmu.requires_write_revalidate,
               (unsigned long long) atomic_load(&mem->mmu.changes));

    read_lock(&mem->lock);
    // Every guest page in the same host page, so a 16 KB host page shows all
    // four. Falls back to the faulting page alone if the host page is smaller.
    size_t hp = real_page_size > PAGE_SIZE ? real_page_size : PAGE_SIZE;
    guest_addr_t host_base = addr & ~(guest_addr_t) (hp - 1);
    printk("arm64 panel: retry=%u addr=%#llx resolved=%p hostpage=%#llx\n",
           retry, (unsigned long long) addr, resolved,
           (unsigned long long) host_base);
    for (guest_addr_t a = host_base; a < host_base + hp; a += PAGE_SIZE) {
        struct pt_entry *pt = mem_pt(mem, PAGE(a));
        if (pt == NULL) {
            printk("  %#llx unmapped%s\n", (unsigned long long) a,
                   PAGE(a) == PAGE(addr) ? "   <== faulting" : "");
            continue;
        }
        printk("  %#llx flags=%#x W=%d COW=%d anon=%d shared=%d off=%#zx "
               "data=%p refs=%u protcache=%s%s\n",
               (unsigned long long) a, pt->flags,
               !!(pt->flags & P_WRITE), !!(pt->flags & P_COW),
               !!(pt->flags & P_ANONYMOUS), !!(pt->flags & P_SHARED),
               pt->offset, (void *) pt->data,
               pt->data != NULL ? (unsigned) pt->data->refcount : 0u,
               (pt->data != NULL && pt->data->host_page_prot != NULL) ? "yes" : "no",
               PAGE(a) == PAGE(addr) ? "   <== faulting" : "");
    }
    read_unlock(&mem->lock);
}

guest_addr_t fork_fault_untag(struct cpu_state *cpu, guest_addr_t ip) {
    // AArch64 TBI: bits 56-63 are not part of the address, so the fault has to
    // be RESOLVED against the untagged one. This is not a workaround for a
    // missing mask somewhere upstream of here -- it is what Linux's own
    // do_page_fault does, on its very first line, before it has any idea which
    // vma the access belongs to (`addr = untagged_addr(far)`). A tagged
    // address that reaches this function is a perfectly ordinary event on a
    // guest whose allocator tags pointers: every copy-on-write break and every
    // first touch of a lazily-backed page arrives here, and resolving the
    // literal 64-bit value would fail every one of them.
    //
    // si_addr still reports what the guest computed, tag and all -- also what
    // Linux reports, because it hands the raw FAR to the signal frame.
    guest_addr_t fault_addr = cpu->segfault_addr;
    if (current->abi == GUEST_ABI_ARM64) {
        fault_addr = (guest_addr_t)
            guest_abi_untag_addr(GUEST_ABI_ARM64, cpu->segfault_addr);
        // Still worth saying out loud, rate-limited: everything BELOW this
        // function is supposed to have stripped the tag already (the JIT's
        // read_prep/write_prep do it before the TLB index, the syscall
        // helpers before the range check), so a tagged address surfacing here
        // means one path did not, and the recovery above is hiding it.
        static unsigned tagged_fault_log_count;
        enum { TAGGED_FAULT_LOG_BUDGET = 8 };
        if (fault_addr != cpu->segfault_addr &&
                tagged_fault_log_count < TAGGED_FAULT_LOG_BUDGET) {
            tagged_fault_log_count++;
            printk("arm64: fault address %#llx carries a TBI tag (%#x); "
                   "resolving %#llx at pc %#llx -- the tag should have been "
                   "stripped before it got this far\n",
                   (unsigned long long) cpu->segfault_addr,
                   (unsigned) (cpu->segfault_addr >> 56),
                   (unsigned long long) fault_addr,
                   (unsigned long long) ip);
        }
    }
    return fault_addr;
}

// Bumped on every syscall this thread makes. The re-fault guard below uses it
// as its definition of "the guest made progress": a genuine spin re-executes
// one instruction and makes no syscalls at all, while a program legitimately
// re-faulting at the same address is going round a loop that does (fork, open,
// write). Nothing cheaper separates those two -- see the guard for why elapsed
// time does not.
__thread uint64_t guest_syscall_seq;

void fork_note_interrupt(int interrupt) {
    switch (interrupt) {
        case INT_SYSCALL:
        case INT_AMD64_SYSCALL:
        case INT_ARM64_SVC:
        case INT_RISCV64_ECALL:
            guest_syscall_seq++;
            break;
        default:
            break;
    }
}

// Returns true when the guard trips, having said so; the caller then prints
// the faulting page's state (dump_fault_pt_state, kernel/calls.c):
// "The page is there" is the emulator's conclusion, not a fact
// anyone has checked. This is the one place that can print what it
// is actually looking at, and every remaining theory about this
// loop is a claim about these flags: whether P_WRITE is set,
// whether P_COW survived the break, and whether the entry is even
// the one the store is addressing.
//
// Five device failures reduce to this loop (socket_kill,
// accept_kill, clone_error_cleanup, fifo_open_creat_deadlock,
// concurrent_exec_tlb, all at the same musl prologue push), and
// four repro attempts could not reach it off-device -- host page
// mirroring is compiled around real_page_size == PAGE_SIZE, so an
// x86_64 harness does not even run the same branches. The only way
// this gets answered is from the device, which means printing it
// here rather than reasoning about it anywhere else.
bool fork_fault_spinning(struct cpu_state *cpu, guest_addr_t fault_ip) {
    // A fault that RESOLVES and then immediately re-faults at the same pc and
    // the same address is not progress, it is a loop -- and an emulator that
    // loops is strictly worse than one that crashes. A crash names a pc and
    // leaves a log; a loop wedges the app until iOS kills it, taking the
    // kernel log with it, and every measurement after that is guesswork.
    //
    // Observed for real, and caused by the untagging above: the tagged fault
    // resolved, the guest re-executed the faulting instruction, the access
    // presented the same tagged address again, and the pair spun. Resolution
    // succeeding is not the same as the access being able to complete, and
    // where it isn't, falling back to the signal is the honest answer -- it is
    // also exactly what happened before the untagging existed.
    static __thread guest_addr_t last_fault_addr;
    static __thread guest_addr_t last_fault_ip;
    static __thread unsigned same_fault_count;
    enum { SAME_FAULT_LIMIT = 16 };
    // A genuine spin re-executes one instruction and makes NO SYSCALLS. A
    // program that legitimately re-faults at the same address is going round a
    // loop that does (fork, open, write), so its syscall count moves between
    // faults. That is the discriminator.
    //
    // Elapsed time was tried first and is NOT good enough. A 20ms window fixed
    // socket_kill, accept_kill and concurrent_exec_tlb, and left
    // clone_error_cleanup and fifo_open_creat_deadlock still dying: both loop
    // tightly enough to fit sixteen legitimate copy-on-write faults inside
    // 20ms. Any threshold wide enough for them would be wide enough to let a
    // real spin through, because "fast" is not what distinguishes these.
    //
    // That distinction is the whole correctness of this guard, and leaving it
    // out cost five device test failures. Matching on address and ip alone,
    // socket_kill and four others were killed by a SPURIOUS SIGSEGV: musl's
    // fork() pushes the same callee prologue frame at the same stack address
    // every call, so a program that forks in a loop produces an identical
    // (addr, ip) fault each time -- sixteen healthy copy-on-write breaks, each
    // resolved correctly, counted as a spin. The device log showed FOUR MINUTES
    // between two of the "retries", and the faulting page reading
    // `W=1 COW=1 refs=2`: an ordinary shared post-fork page, resolving fine.
    //
    // Resetting on successful resolution instead would be wrong and would
    // disable the guard outright: the tagged-pointer bug this exists for also
    // resolved successfully every time and re-faulted anyway. Progress is the
    // discriminator, not resolution.
    static __thread uint64_t last_fault_syscall_seq;
    bool looping = false;
    if (cpu->segfault_addr == last_fault_addr && fault_ip == last_fault_ip &&
            guest_syscall_seq == last_fault_syscall_seq) {
        // Every time round, so the panel shows whether the state EVOLVES
        // across retries (a race) or is identical sixteen times (a logic bug).
        if (current != NULL && current->abi == GUEST_ABI_ARM64)
            dump_arm64_fault_panel(cpu->segfault_addr, same_fault_count, NULL);
        if (++same_fault_count > SAME_FAULT_LIMIT) {
            looping = true;
            same_fault_count = 0;
            printk("ERROR: %d(%s) [%s] fault on %#llx at %#llx keeps resolving "
                   "and re-faulting (%d times); delivering SIGSEGV rather than "
                   "spinning -- the access cannot complete even though the page "
                   "is there\n",
                   current->pid, current->comm, guest_abi_desc(current->abi).name,
                   (unsigned long long) cpu->segfault_addr,
                   (unsigned long long) fault_ip, SAME_FAULT_LIMIT);
        }
    } else {
        last_fault_addr = cpu->segfault_addr;
        last_fault_ip = fault_ip;
        same_fault_count = 0;
    }
    last_fault_syscall_seq = guest_syscall_seq;
    return looping;
}

void fork_arm64_fault_report(struct cpu_state *cpu, guest_addr_t fault_addr, bool looping, void *ptr) {
    // A fault address with a top byte set is a TBI-tagged pointer that
    // reached somewhere it should have been untagged. Saying so here
    // costs one line and is the difference between "a wild pointer"
    // and "one missing mask" -- which is exactly how long the first
    // one of these took to work out.
    //
    // Two very different things end up here now, and confusing them
    // costs a session: the untagged address being unmapped too (a
    // genuine wild pointer, the tag is not what killed it), or the
    // untagged address being perfectly fine and the access still not
    // completing (the loop cap above tripped -- a real emulator bug,
    // and the tag IS implicated). Say which.
    if (fault_addr != cpu->segfault_addr)
        printk("  TAGGED POINTER: tag=%#x untagged=%#llx (%s)\n",
               (unsigned) (cpu->segfault_addr >> 56),
               (unsigned long long) fault_addr,
               looping
                   ? "MAPPED -- resolved and re-faulted until the loop "
                     "cap tripped, so something below here is still "
                     "seeing the tag"
                   : "unmapped as well, so the tag is not what killed it");
    dump_arm64_fault_panel(cpu->segfault_addr, 0, ptr);
}

void fork_arm64_fault_detail(char *out, size_t size, const char *opcode, const struct cpu_state *cpu) {
    // arm64 had no branch here at all, so a fault on THIS FORK'S SHIPPING
    // ARCHITECTURE fell through to the i386 case and printed eax/eip out of
    // a 32-bit register view of an aarch64 task -- numbers that are not
    // merely useless but actively misleading.
    //
    // x30 is the one that matters most and is the reason this was noticed.
    // A null-pointer atomic faults inside an outline-atomics helper
    // (__aarch64_ldadd4_relax and friends), which builds no frame, so the
    // only record of who asked for it is the link register. Without x30 a
    // crash in one of those stubs is unattributable, which is exactly the
    // state idmap2d's fault was in.
    snprintf(out, size,
             "opcode window: %s\n"
             "x0=%#llx x1=%#llx x2=%#llx x3=%#llx\n"
             "x29(fp)=%#llx x30(lr)=%#llx sp=%#llx pc=%#llx",
             opcode,
             (unsigned long long) cpu->arm64_regs[arm64_x0],
             (unsigned long long) cpu->arm64_regs[arm64_x1],
             (unsigned long long) cpu->arm64_regs[arm64_x2],
             (unsigned long long) cpu->arm64_regs[arm64_x3],
             (unsigned long long) cpu->arm64_regs[arm64_x29],
             (unsigned long long) cpu->arm64_regs[arm64_x30],
             (unsigned long long) cpu->arm64_sp,
             (unsigned long long) cpu->arm64_pc);
}
