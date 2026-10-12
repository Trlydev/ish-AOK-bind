// /proc/ish/jit_cache, the switch behind the app's "Cache Translated Code"
// setting: translated code kept beyond the process that made it (jit/jit.c's
// exec cache). A cached block is found by the address its code was at, so the
// cache is only any use when every process is laid out the same way -- and
// the switch is the one place that trade is made:
//
//   - it is off unless something turns it on, and the layout is random;
//   - on: /proc/sys/kernel/randomize_va_space reads 0 and the layout stops
//     moving between runs, and (arm64 guests, the only ones it caches for)
//     running the same program again is served from the cache;
//   - off: randomize_va_space is what it was, the layout moves again, and
//     the cache is empty;
//   - off does not undo a randomize_va_space somebody else set meanwhile.
//
// Needs root (it writes under /proc); skips on a kernel without the file.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CACHE "/proc/ish/jit_cache"
#define ASLR "/proc/sys/kernel/randomize_va_space"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("jit_cache_switch: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static int read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return 0;
}
static int write_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, value, strlen(value));
    close(fd);
    return n == (ssize_t) strlen(value) ? 0 : -1;
}
static int aslr(void) {
    char buf[16];
    return read_file(ASLR, buf, sizeof(buf)) == 0 ? atoi(buf) : -1;
}
static long cache_field(const char *name) {
    char buf[256];
    if (read_file(CACHE, buf, sizeof(buf)) != 0)
        return -1;
    if (name == NULL)
        return atoi(buf);
    char *at = strstr(buf, name);
    return at != NULL ? atol(at + strlen(name)) : -1;
}

// Where a fresh copy of this program finds its stack and a new mapping.
static int layout(const char *self, uint64_t out[2]) {
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        execl(self, self, "layout", (char *) NULL);
        _exit(127);
    }
    close(fds[1]);
    char buf[128];
    ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    int status;
    waitpid(pid, &status, 0);
    if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    buf[n] = '\0';
    unsigned long long a, b;
    if (sscanf(buf, "%llx %llx", &a, &b) != 2)
        return -1;
    out[0] = a;
    out[1] = b;
    return 0;
}
// How many of `runs` fresh copies had the same layout as the first.
static int same_layouts(const char *self, int runs) {
    uint64_t first[2], now[2];
    if (layout(self, first) != 0)
        return -1;
    int same = 0;
    for (int i = 1; i < runs; i++) {
        if (layout(self, now) != 0)
            return -1;
        same += now[0] == first[0] && now[1] == first[1];
    }
    return same;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "layout") == 0) {
        int local;
        void *map = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        printf("%llx %llx\n", (unsigned long long) (uintptr_t) &local & ~0xfffull, (unsigned long long) (uintptr_t) map);
        return 0;
    }
    if (access(CACHE, F_OK) != 0) {
        printf("jit_cache_switch: SKIP (no " CACHE ")\n");
        return 0;
    }
    if (geteuid() != 0) {
        printf("jit_cache_switch: SKIP (needs root)\n");
        return 0;
    }
    const char *self = "/proc/self/exe";
    int was_on = (int) cache_field(NULL), aslr_before = aslr();
    if (was_on != 0) {
        // Somebody runs this root with the cache on (the app's setting, or
        // ISH_JIT_XCACHE): put it as found when done, and start from off.
        write_file(CACHE, "0");
    }
    int aslr_off = aslr();
    CHECK(cache_field(NULL) == 0, "off reads %ld", cache_field(NULL));

    // Off: the layout is whatever randomize_va_space says. (Under
    // ISH_RANDOMIZE_VA_SPACE=0 it does not move, and there is less to see.)
    if (aslr_off == 2)
        CHECK(same_layouts(self, 6) < 5, "with the cache off and randomize_va_space 2 the layout did not move");

    // On: no randomization, and the same code again comes from the cache.
    CHECK(write_file(CACHE, "1") == 0, "writing 1 failed");
    CHECK(cache_field(NULL) == 1, "on reads %ld", cache_field(NULL));
    CHECK(aslr() == 0, "with the cache on randomize_va_space reads %d", aslr());
    CHECK(same_layouts(self, 6) == 5, "with the cache on the layout still moves");
#if defined(__aarch64__)
    long hits = cache_field("hits ");
    CHECK(same_layouts(self, 4) == 3, "layouts, second batch");
    long hits_after = cache_field("hits ");
    CHECK(hits_after > hits + 100, "four more runs of the same program: cache hits %ld -> %ld", hits, hits_after);
    CHECK(cache_field("bytes ") > 0, "the cache is on and holds nothing");
#endif

    // Off again: as it was, and empty.
    CHECK(write_file(CACHE, "0") == 0, "writing 0 failed");
    CHECK(aslr() == aslr_off, "after off randomize_va_space reads %d, was %d", aslr(), aslr_off);
    CHECK(cache_field("bytes ") == 0, "after off the cache still holds %ld bytes", cache_field("bytes "));
    if (aslr_off == 2)
        CHECK(same_layouts(self, 6) < 5, "after off the layout does not move");

    // Somebody else's setting made while the cache was on is theirs.
    write_file(CACHE, "1");
    CHECK(write_file(ASLR, "1") == 0, "writing randomize_va_space failed");
    write_file(CACHE, "0");
    CHECK(aslr() == 1, "off undid a randomize_va_space set meanwhile: reads %d", aslr());
    char back[4];
    snprintf(back, sizeof(back), "%d", aslr_off);
    write_file(ASLR, back);

    // Not 0 or 1 is refused.
    CHECK(write_file(CACHE, "2") != 0, "writing 2 was accepted");

    if (was_on == 1) {
        write_file(CACHE, "1");
    } else if (aslr_before != aslr()) {
        snprintf(back, sizeof(back), "%d", aslr_before);
        write_file(ASLR, back);
    }
    if (failures != 0) {
        printf("jit_cache_switch: FAIL failures=%d\n", failures);
        return 1;
    }
    printf("jit_cache_switch: PASS\n");
    return 0;
}
