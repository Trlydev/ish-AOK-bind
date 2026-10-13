// A guest may open as many files as its own limit allows, whatever soft
// limit the host started the emulator with.
//
// Every guest descriptor that is a real file, socket or pipe is a host
// descriptor, and all guest processes share the one host process. The host
// starts that process with a soft limit of its choosing -- 256 from launchd
// for an app, 256 from sshd for a suite leg -- and nothing raised it: a guest
// well inside its own RLIMIT_NOFILE (1024) got EMFILE from the host at a
// count it could not see, and a sendmsg that met that state took the
// emulator down (scm_rights_bad_fd.c). mount_root now raises the soft limit
// as far as the host allows, and /proc/ish/host_fds reports the count, the
// limits, and the one the process was started with.
//
// This opens one file 700 times -- more than a 256 start allows, fewer than
// the guest's own limit -- and expects every open to succeed. It reads
// /proc/ish/host_fds before and after, and the count must have risen by at
// least 600: that is what says these really are host descriptors, so that a
// pass here is a pass of the thing claimed. Where the host's HARD limit is
// too low to hold them, it says so and skips.
//
// A leg started with `ulimit -n 256` (every leg on the MBP is) fails this on
// a build without the raise; a shell with a large limit passes either way,
// and the `started` line in the output says which it was.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OPENS 700

struct usage { long open, highest; long long soft, hard, started; };

static long long limit_value(const char *text) {
    return strncmp(text, "unlimited", 9) == 0 ? -1 : atoll(text);
}

static int read_usage(struct usage *u) {
    FILE *f = fopen("/proc/ish/host_fds", "r");
    if (f == NULL)
        return -1;
    char line[128];
    memset(u, 0, sizeof(*u));
    int seen = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char name[32], value[64];
        if (sscanf(line, "%31s %63s", name, value) != 2)
            continue;
        if (strcmp(name, "open") == 0) { u->open = atol(value); seen++; }
        else if (strcmp(name, "highest") == 0) { u->highest = atol(value); seen++; }
        else if (strcmp(name, "soft_limit") == 0) { u->soft = limit_value(value); seen++; }
        else if (strcmp(name, "hard_limit") == 0) { u->hard = limit_value(value); seen++; }
        else if (strcmp(name, "started") == 0) { u->started = limit_value(value); seen++; }
    }
    fclose(f);
    return seen == 5 ? 0 : -2; // (the by-type lines are not read here)
}

int main(void) {
    struct usage before, after;
    int err = read_usage(&before);
    if (err == -1) {
        printf("host_fd_limit: SKIP (no /proc/ish/host_fds)\n");
        return 0;
    }
    if (err != 0) {
        printf("host_fd_limit: FAIL /proc/ish/host_fds does not have its five lines\n");
        return 1;
    }
    if (before.hard >= 0 && before.hard < before.open + OPENS + 64) {
        printf("host_fd_limit: SKIP (the host's hard limit is %lld)\n", before.hard);
        return 0;
    }

    char path[] = "/tmp/host_fd_limit.XXXXXX";
    int first = mkstemp(path);
    if (first < 0) {
        printf("host_fd_limit: FAIL mkstemp: %s\n", strerror(errno));
        return 1;
    }
    static int fds[OPENS];
    int opened = 0, open_errno = 0;
    for (; opened < OPENS; opened++) {
        fds[opened] = open(path, O_RDONLY);
        if (fds[opened] < 0) {
            open_errno = errno;
            break;
        }
    }
    int failures = 0;
    if (opened != OPENS) {
        printf("host_fd_limit: open number %d failed: %s (the host started this process with a limit of %lld, now %lld)\n",
               opened + 1, strerror(open_errno), before.started, before.soft);
        failures++;
    }
    if (read_usage(&after) != 0) {
        printf("host_fd_limit: /proc/ish/host_fds could not be read with %d files open\n", opened);
        failures++;
    } else if (opened == OPENS && after.open - before.open < 600) {
        printf("host_fd_limit: %d opens raised the host's count by only %ld (%ld -> %ld): these are not host descriptors, and the test proves nothing\n",
               OPENS, after.open - before.open, before.open, after.open);
        failures++;
    }
    if (before.soft >= 0 && before.started >= 0 && before.soft < before.started) {
        printf("host_fd_limit: the soft limit went DOWN, from %lld to %lld\n", before.started, before.soft);
        failures++;
    }
    for (int i = 0; i < opened; i++)
        close(fds[i]);
    close(first);
    unlink(path);

    if (failures != 0) {
        printf("host_fd_limit: FAIL failures=%d\n", failures);
        return 1;
    }
    printf("host_fd_limit: PASS (started %lld, now %lld; %ld host descriptors with %d files open)\n",
           before.started, before.soft, after.open, OPENS);
    return 0;
}
