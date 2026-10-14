// RLIMIT_NPROC across a user namespace: what a namespace's processes may
// number is bounded by its creator's limit outside, as it was when the
// namespace was made -- raising the soft limit inside buys nothing.
//
// Linux counts processes per (user namespace, real uid) and charges each one
// at every level up to the initial namespace, against the creator's limit
// there (ucounts: inc_rlimit_ucounts, is_rlimit_overlimit). So with a soft
// limit of base+10 and a hard one of base+200 outside, a child that makes a
// user namespace and raises its soft limit to base+200 inside can still fork
// only as many as the parent could, less itself: Linux 6.12 says 10 outside
// and 9 inside. AOK counted the uid machine-wide against the task's own
// (raised) limit, and allowed 199. Raising the hard limit inside is EPERM.
//
// The counts are compared with each other, not with fixed numbers: what else
// the uid is running moves both alike. Run as root it becomes a uid nothing
// else uses; as anyone else, it is that user.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <grp.h>
#include <sys/wait.h>
#include <unistd.h>
static void wf(const char *p, const char *t) { int fd = open(p, O_WRONLY); if (fd >= 0) { if (write(fd, t, strlen(t)) < 0) {} close(fd); } }
// Threads of the real uid, machine-wide (what RLIMIT_NPROC counts).
static int count_uid(uid_t uid) {
    int n = 0; DIR *d = opendir("/proc"); struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[300]; snprintf(p, sizeof p, "/proc/%s/task", e->d_name);
        DIR *t = opendir(p); if (!t) continue; struct dirent *te;
        while ((te = readdir(t))) {
            if (te->d_name[0] < '0' || te->d_name[0] > '9') continue;
            char sp[600], line[256]; snprintf(sp, sizeof sp, "%s/%s/status", p, te->d_name);
            FILE *f = fopen(sp, "r"); if (!f) continue;
            while (fgets(line, sizeof line, f)) { unsigned r; if (sscanf(line, "Uid: %u", &r) == 1) { if (r == uid) n++; break; } }
            fclose(f);
        }
        closedir(t);
    }
    closedir(d); return n;
}
// Fork sleepers until fork fails; their number, and the errno.
static int fill(int *err) {
    pid_t kids[2000]; int k = 0;
    for (; k < 2000; k++) {
        pid_t p = fork();
        if (p == 0) { pause(); _exit(0); }
        if (p < 0) { *err = errno; break; }
        kids[k] = p;
    }
    for (int i = 0; i < k; i++) kill(kids[i], SIGKILL);
    for (int i = 0; i < k; i++) waitpid(kids[i], NULL, 0);
    return k;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    uid_t uid = getuid(); gid_t gid = getgid();
    if (uid == 0) {
        uid = gid = 47000 + (unsigned) getpid() % 1000;
        if (setgroups(0, NULL) || setgid(gid) || setuid(uid)) { printf("userns_nproc: FAIL setup: setuid: %s\n", strerror(errno)); return 1; }
    }
    int base = count_uid(uid);
    struct rlimit rl = {.rlim_cur = base + 10, .rlim_max = base + 200};
    if (setrlimit(RLIMIT_NPROC, &rl)) { printf("userns_nproc: FAIL setup: setrlimit: %s\n", strerror(errno)); return 1; }
    int e0 = 0, k0 = fill(&e0);
    if (e0 != EAGAIN || k0 < 2) {
        printf("userns_nproc: FAIL outside, %d forks then %s; wanted some, then EAGAIN\n", k0, strerror(e0));
        return 1;
    }
    int pfd[2]; if (pipe(pfd)) return 1;
    pid_t c = -1;
    for (int tries = 0; tries < 200 && (c = fork()) < 0; tries++)
        usleep(10000);   // (the sleepers' exits are counted off a moment later)
    if (c < 0) { printf("userns_nproc: FAIL setup: fork: %s\n", strerror(errno)); return 1; }
    if (c == 0) {
        char m[64];
        if (unshare(CLONE_NEWUSER)) { printf("userns_nproc: SKIP (no user namespace: %s)\n", strerror(errno)); _exit(2); }
        wf("/proc/self/setgroups", "deny");
        snprintf(m, sizeof m, "0 %u 1", uid); wf("/proc/self/uid_map", m);
        snprintf(m, sizeof m, "0 %u 1", gid); wf("/proc/self/gid_map", m);
        struct rlimit up = {.rlim_cur = rl.rlim_max, .rlim_max = rl.rlim_max};
        int se = setrlimit(RLIMIT_NPROC, &up) ? errno : 0;
        int e1 = 0, k1 = fill(&e1);
        int bad = 0;
        if (se != 0) {
            printf("userns_nproc: raising the soft limit to the hard one inside failed: %s\n", strerror(se));
            bad++;
        }
        // (The child itself is one of the parent's.)
        if (k1 != k0 - 1 || e1 != EAGAIN) {
            printf("userns_nproc: inside, %d forks then %s; wanted %d (the %d outside, less this one), then EAGAIN\n",
                    k1, strerror(e1), k0 - 1, k0);
            bad++;
        }
        struct rlimit hi = {.rlim_cur = rl.rlim_max + 100, .rlim_max = rl.rlim_max + 100};
        errno = 0;
        if (setrlimit(RLIMIT_NPROC, &hi) == 0 || errno != EPERM) {
            printf("userns_nproc: raising the hard limit inside did not fail with EPERM (%s)\n", strerror(errno));
            bad++;
        }
        _exit(bad ? 1 : 0);
    }
    int status = -1;
    waitpid(c, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
        return 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("userns_nproc: FAIL\n");
        return 1;
    }
    printf("userns_nproc: PASS\n");
    return 0;
}
