// ISH_JIT_PROFILE=<file>: the dynamic instruction mix of arm64 and riscv64
// guest code, for deciding what to fuse or translate natively.
//
// Every guest instruction costs about the same under the gadget JIT (~1.5 ns
// on an M5, whatever it does), so what a workload spends is set by how many
// of each it runs. Guest PC sampling cannot say: the PC is published only when
// execution returns to the dispatcher, and chained blocks never do. So each
// translated block starts with a gadget that bumps its own counter, and keeps
// the guest instruction words it was made from. At exit the file gets one
// line per block that ran: "<abi> <count> <addr> <insn> <insn> ...", and
// tools/jitprof-report.py turns that into the mix, the run lengths of vector
// instructions, and the commonest adjacent pairs.
//
// Blocks are never freed (a translated block that is thrown away and made
// again gets a second record); the counters race between threads. Both are
// fine for a measurement and would not be for anything else.

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jit/jitprof.h"

struct jitprof_block {
    uint64_t count;               // bumped by the block_count gadget
    uint64_t addr;
    struct jitprof_block *next;
    uint32_t *insns;
    uint32_t n, cap;
    uint8_t abi;
    // x86: consumed bytes as [len][bytes]... records.
    uint8_t *bytes;
    uint32_t nb, capb;
    // arm64: what each translation step emitted, as [first gadget][words]
    // [instructions consumed] triples -- the gadget-level view of the block.
    uint64_t *steps;
    uint32_t ns, caps;
};

static pthread_once_t jitprof_once = PTHREAD_ONCE_INIT;
static const char *jitprof_path;
static pthread_mutex_t jitprof_lock = PTHREAD_MUTEX_INITIALIZER;
static struct jitprof_block *jitprof_blocks;

// A gadget's name, less its prefix. dladdr searches the symbol table each
// time, and a profile names millions of gadgets from a few hundred, so the
// answers are kept (only jitprof_dump calls this, under jitprof_lock).
static const char *gadget_name(uint64_t gadget) {
    static struct { uint64_t gadget; const char *name; } cache[4096];
    unsigned slot = (unsigned) ((gadget * 0x9e3779b97f4a7c15ull) >> 52);
    for (unsigned n = 0; n < 4096; n++, slot = (slot + 1) & 4095) {
        if (cache[slot].gadget == gadget)
            return cache[slot].name;
        if (cache[slot].name != NULL)
            continue;
        Dl_info info;
        const char *sym = "?";
        if (dladdr((void *) (uintptr_t) gadget, &info) && info.dli_sname != NULL &&
                (uintptr_t) info.dli_saddr == (uintptr_t) gadget)
            sym = info.dli_sname;
        if (*sym == '_')
            sym++;
        if (strncmp(sym, "gadget_arm64_", 13) == 0)
            sym += 13;
        else if (strncmp(sym, "gadget_riscv64_", 15) == 0)
            sym += 15;
        cache[slot].gadget = gadget;
        cache[slot].name = sym;
        return sym;
    }
    return "?";
}

// Called from main.c's cli_halt, as the other instruments' dumps are (it
// exits with _exit, so atexit would never run). No-op unless ISH_JIT_PROFILE.
void jitprof_dump(void) {
    if (jitprof_path == NULL)
        return;
    FILE *f = fopen(jitprof_path, "w");
    if (f == NULL)
        return;
    pthread_mutex_lock(&jitprof_lock);
    for (struct jitprof_block *b = jitprof_blocks; b != NULL; b = b->next) {
        uint64_t count = __atomic_load_n(&b->count, __ATOMIC_RELAXED);
        if (count == 0 || (b->n == 0 && b->nb == 0))
            continue;
        static const char *const names[] = {
            [JITPROF_ARM64] = "arm64", [JITPROF_RISCV64] = "riscv64",
            [JITPROF_I386] = "i386", [JITPROF_AMD64] = "amd64",
        };
        fprintf(f, "%s %llu %#llx", names[b->abi],
                (unsigned long long) count, (unsigned long long) b->addr);
        for (uint32_t i = 0; i < b->n; i++)
            fprintf(f, " %08x", b->insns[i]);
        // x86: one hex token per translation step (a fused step may hold
        // several instructions; the report splits them).
        for (uint32_t i = 0; i < b->nb; ) {
            unsigned len = b->bytes[i++];
            fputc(' ', f);
            for (unsigned j = 0; j < len && i < b->nb; j++)
                fprintf(f, "%02x", b->bytes[i++]);
        }
        fputc('\n', f);
        if (b->ns == 0)
            continue;
        // "<abi>g <count> <addr> <gadget>/<words>/<instructions> ...": the
        // same block as the gadgets it became (tools/jitprof-gadget-pairs.py).
        fprintf(f, "%sg %llu %#llx", names[b->abi],
                (unsigned long long) count, (unsigned long long) b->addr);
        for (uint32_t i = 0; i + 2 < b->ns; i += 3)
            fprintf(f, " %s/%llu/%llu", gadget_name(b->steps[i]),
                    (unsigned long long) b->steps[i + 1], (unsigned long long) b->steps[i + 2]);
        fputc('\n', f);
    }
    pthread_mutex_unlock(&jitprof_lock);
    fclose(f);
}

static void jitprof_init(void) {
    const char *path = getenv("ISH_JIT_PROFILE");
    if (path == NULL || *path == '\0')
        return;
    jitprof_path = strdup(path);
}

struct jitprof_block *jitprof_block_new(uint64_t addr, enum jitprof_abi abi) {
    pthread_once(&jitprof_once, jitprof_init);
    if (jitprof_path == NULL)
        return NULL;
    struct jitprof_block *b = calloc(1, sizeof(*b));
    if (b == NULL)
        return NULL;
    b->addr = addr;
    b->abi = (uint8_t) abi;
    pthread_mutex_lock(&jitprof_lock);
    b->next = jitprof_blocks;
    jitprof_blocks = b;
    pthread_mutex_unlock(&jitprof_lock);
    return b;
}

uint64_t *jitprof_counter(struct jitprof_block *block) {
    return &block->count;
}

void jitprof_note_bytes(struct jitprof_block *block, const uint8_t *bytes, unsigned len) {
    if (len == 0 || len > 255)
        return;
    if (block->nb + len + 1 > block->capb) {
        uint32_t cap = block->capb ? block->capb * 2 : 64;
        while (cap < block->nb + len + 1)
            cap *= 2;
        uint8_t *b = realloc(block->bytes, cap);
        if (b == NULL)
            return;
        block->bytes = b;
        block->capb = cap;
    }
    block->bytes[block->nb++] = (uint8_t) len;
    memcpy(block->bytes + block->nb, bytes, len);
    block->nb += len;
}

void jitprof_note(struct jitprof_block *block, uint32_t insn) {
    if (block->n == block->cap) {
        uint32_t cap = block->cap ? block->cap * 2 : 16;
        uint32_t *insns = realloc(block->insns, cap * sizeof(*insns));
        if (insns == NULL)
            return;
        block->insns = insns;
        block->cap = cap;
    }
    block->insns[block->n++] = insn;
}

void jitprof_note_step(struct jitprof_block *block, uint64_t gadget, unsigned words, unsigned insns) {
    if (block->ns + 3 > block->caps) {
        uint32_t cap = block->caps ? block->caps * 2 : 48;
        uint64_t *steps = realloc(block->steps, cap * sizeof(*steps));
        if (steps == NULL)
            return;
        block->steps = steps;
        block->caps = cap;
    }
    block->steps[block->ns++] = gadget;
    block->steps[block->ns++] = words;
    block->steps[block->ns++] = insns;
}

void jitprof_note_fused(struct jitprof_block *block, unsigned how) {
    if (block != NULL && block->ns >= 3)
        block->steps[block->ns - 1] += 100 * how;
}
