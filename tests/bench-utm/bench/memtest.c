// memtest.c -- loads and stores where an engine's fast path does not apply.
//
// A transcript, like check.sh's: every line must be the same whichever engine ran it. What it covers that
// the programs of check.sh reach only by accident:
//
//   sweep   every size, signed and unsigned, at every alignment, in a page and across a page boundary,
//           with values whose sign bit is set
//   fault   accesses to pages that are not there yet: a SIGSEGV handler opens the page and returns, so the
//           instruction runs again. The loops advance a pointer and then use it, so an engine that runs two
//           guest instructions as one step must not repeat the first of them, and must report the second's
//           address. The handler hashes every fault address and the pc's distance from the last one.
//   cross   the same, for an access whose first bytes are in a mapped page and whose last are not
//
// Built -O2 and with plain pointer dereferences on purpose: the compiler then emits one load or store
// instruction per access, misaligned or not.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

static size_t page;
static unsigned char *region;
static size_t region_size;
static volatile uint64_t fault_hash;
static volatile unsigned long faults;
static volatile uintptr_t last_pc;

static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

static uintptr_t context_pc(void *uc_) {
    ucontext_t *uc = uc_;
#if defined(__riscv)
    return uc->uc_mcontext.__gregs[0];
#elif defined(__aarch64__)
    return uc->uc_mcontext.pc;
#else
    (void) uc;
    return 0;
#endif
}

static void on_segv(int sig, siginfo_t *si, void *uc) {
    uintptr_t addr = (uintptr_t) si->si_addr, pc = context_pc(uc);
    (void) sig;
    if (addr < (uintptr_t) region || addr >= (uintptr_t) region + region_size)
        _exit(3);
    faults++;
    // Where the fault was, and how far the pc moved since the last one: both depend on exactly which
    // instruction was restarted, neither on where the program was loaded.
    fault_hash = mix(fault_hash, addr - (uintptr_t) region);
    fault_hash = mix(fault_hash, last_pc ? pc - last_pc : 0);
    last_pc = pc;
    if (mprotect((void *) (addr & ~(page - 1)), page, PROT_READ | PROT_WRITE) != 0)
        _exit(4);
}

static void fresh_region(size_t pages, int prot) {
    if (region != NULL)
        munmap(region, region_size);
    region_size = pages * page;
    region = mmap(NULL, region_size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED)
        _exit(2);
    fault_hash = 0;
    faults = 0;
    last_pc = 0;
}

#define TYPES(X) X(int8_t) X(uint8_t) X(int16_t) X(uint16_t) X(int32_t) X(uint32_t) X(int64_t) X(uint64_t)

// One load and one store of each type, each its own function so that the access is a single instruction
// the caller's surroundings cannot fold away.
#define ACCESSORS(T) \
    static __attribute__((noinline)) uint64_t load_##T(const unsigned char *p) { \
        return (uint64_t) (int64_t) *(const volatile T *) p; \
    } \
    static __attribute__((noinline)) void store_##T(unsigned char *p, uint64_t v) { \
        *(volatile T *) p = (T) v; \
    }
TYPES(ACCESSORS)

static uint64_t sweep(void) {
    uint64_t h = 1;
    fresh_region(4, PROT_READ | PROT_WRITE);
    for (size_t i = 0; i < region_size; i++)
        region[i] = (unsigned char) (0x80 | (i * 131 + (i >> 8) * 17 + 7));
    // Around the boundary between the second and third pages, and around the middle of the first.
    for (int where = 0; where < 2; where++) {
        unsigned char *base = where == 0 ? region + 2 * page - 24 : region + 512;
        for (int off = 0; off < 48; off++) {
            unsigned char *p = base + off;
#define SWEEP(T) \
            h = mix(h, load_##T(p)); \
            store_##T(p, ~h ^ ((uint64_t) off << 56)); \
            h = mix(h, load_##T(p)); \
            h = mix(h, load_uint64_t(p - 8) ^ load_uint64_t(p + 1));
            TYPES(SWEEP)
        }
    }
    for (size_t i = 0; i < region_size; i++)
        h = mix(h, region[i]);
    return h;
}

// Pointer walks over pages that fault on first touch. The pointer is advanced and then used, or used and
// then advanced, by a stride that lands in a new page every few steps.
#define WALKS(T) \
    static __attribute__((noinline)) uint64_t walk_store_##T(unsigned char *p, long stride, long n) { \
        uint64_t v = 0x8123456789abcdefull; \
        for (long i = 0; i < n; i++) { \
            p += stride; \
            *(volatile T *) p = (T) v; \
            v = v * 6364136223846793005ull + 1442695040888963407ull; \
        } \
        return v; \
    } \
    static __attribute__((noinline)) uint64_t walk_load_##T(const unsigned char *p, long stride, long n) { \
        uint64_t h = 7; \
        for (long i = 0; i < n; i++) { \
            T a = *(const volatile T *) p; \
            p += stride; \
            T b = *(const volatile T *) p; \
            h = mix(h, (uint64_t) (int64_t) a + ((uint64_t) (int64_t) b << 1)); \
        } \
        return h; \
    }
TYPES(WALKS)

static uint64_t fault(long stride, long first) {
    uint64_t h = 3;
    long n = (long) ((region_size - 64) / (unsigned long) stride) - 2;
#define FAULT(T) \
    fresh_region(48, PROT_NONE); \
    h = mix(h, walk_store_##T(region + first, stride, n)); \
    h = mix(h, fault_hash); h = mix(h, faults); \
    h = mix(h, walk_load_##T(region + first, stride, n)); \
    fresh_region(48, PROT_NONE); \
    h = mix(h, walk_load_##T(region + first, stride, n)); \
    h = mix(h, fault_hash); h = mix(h, faults); \
    h = mix(h, walk_store_##T(region + first, stride, n)); \
    h = mix(h, walk_load_##T(region + first, stride, n));
    fresh_region(48, PROT_NONE);
    TYPES(FAULT)
    return h;
}

int main(void) {
    struct sigaction sa;
    page = (size_t) sysconf(_SC_PAGESIZE);
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    printf("MEM sweep %016llx\n", (unsigned long long) sweep());
    // Aligned strides, then ones that put every access at an odd address and some across two pages.
    printf("MEM fault 1000 %016llx\n", (unsigned long long) fault(1000, 0));
    printf("MEM fault 2048 %016llx\n", (unsigned long long) fault(2048, 16));
    printf("MEM cross 1365 %016llx\n", (unsigned long long) fault(1365, 3));
    printf("MEM cross 4095 %016llx\n", (unsigned long long) fault(4095, 4093));
    printf("MEM faults %lu\n", faults);
    return 0;
}
