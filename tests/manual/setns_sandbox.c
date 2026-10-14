// setns into a sandbox: joining the user, pid, network and UTS namespaces of
// a process that is in namespaces of its own, which is what nsenter does.
//
// AOK's setns joined mount, UTS and IPC namespaces only: a pid or network
// namespace that was not the initial one was EINVAL, and so was any user
// namespace. With the four kinds a sandbox is made of now real
// (kernel/user_ns.h, kernel/pid_ns.h, kernel/net_ns.h), each can be joined.
//
// As an ordinary user, which is how it runs on Linux as the oracle (6.12,
// camd); as root the test becomes uid 1000 first. The sandbox is a process
// that unshared user, pid, network and UTS namespaces, named its host "box",
// and forked the pid namespace's init, which is the process joined.
//
//   order        before the user namespace is joined, the others are EPERM:
//                each takes CAP_SYS_ADMIN in the namespace that owns it
//   user         joining it makes the caller root there (its owner's uid is
//                what 0 is mapped to); joining it a second time is EINVAL
//   uts          the host name is the sandbox's
//   network      /proc/self/ns/net is the sandbox's
//   pid          the caller keeps its pid and its own namespace; only
//                /proc/self/ns/pid_for_children moves, and the next child is
//                pid 2 in the sandbox with parent 0, and can signal pid 1
//   up           a pid namespace above one's own cannot be joined (EINVAL)
//   pidfd        all four at once through a pidfd
//   ioctls       NS_GET_USERNS on the sandbox's pid namespace is its user
//                namespace, whose NS_GET_OWNER_UID is the uid that made it
//                and whose NS_GET_PARENT is the caller's; the parent of the
//                caller's own namespace is EPERM, and of a kind that does not
//                nest EINVAL
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define NAME "setns_sandbox"
static int failures;
static uid_t outer_uid;
static gid_t outer_gid;

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

#ifndef NS_GET_USERNS
#define NS_GET_USERNS 0xb701
#define NS_GET_PARENT 0xb702
#define NS_GET_OWNER_UID 0xb704
#endif

#define MUST(cond, ...) do { if (!(cond)) { printf(NAME ": " __VA_ARGS__); printf(" (errno %d: %s)\n", errno, strerror(errno)); fflush(stdout); _exit(1 + __LINE__ % 200); } } while (0)

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return errno;
    ssize_t n = write(fd, text, strlen(text));
    int err = n < 0 ? errno : 0;
    close(fd);
    return err;
}

static int wait_status(pid_t pid) {
    int status = -1;
    return waitpid(pid, &status, 0) == pid ? status : -1;
}

static void run(const char *what, void (*body)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        body();
        _exit(0);
    }
    int status = wait_status(pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf(NAME ": %s: child status %#x\n", what, status);
        failures++;
    }
}

// The sandbox: its init's pid as the caller knows it. The init lives until
// `sandbox_hold` is closed.
static pid_t sandbox_init;
static int sandbox_hold = -1;

static void sandbox_start(void) {
    int up[2], hold[2];
    MUST(pipe(up) == 0 && pipe(hold) == 0, "pipe");
    fflush(stdout);
    pid_t maker = fork();
    MUST(maker >= 0, "fork");
    if (maker == 0) {
        close(up[0]);
        close(hold[1]);
        char map[64];
        MUST(unshare(CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWUTS) == 0, "the sandbox's unshare failed");
        MUST(write_file("/proc/self/setgroups", "deny") == 0, "writing setgroups deny failed");
        snprintf(map, sizeof(map), "0 %u 1", (unsigned) outer_uid);
        MUST(write_file("/proc/self/uid_map", map) == 0, "writing the uid map failed");
        snprintf(map, sizeof(map), "0 %u 1", (unsigned) outer_gid);
        MUST(write_file("/proc/self/gid_map", map) == 0, "writing the gid map failed");
        MUST(sethostname("box", 3) == 0, "sethostname in the sandbox failed");
        pid_t init = fork();
        MUST(init >= 0, "fork of the sandbox's init failed");
        if (init == 0) {
            char c;
            (void) !read(hold[0], &c, 1);
            _exit(0);
        }
        MUST(write(up[1], &init, sizeof(init)) == sizeof(init), "write");
        close(up[1]);
        close(hold[0]);
        _exit(wait_status(init) == 0 ? 0 : 9);
    }
    close(up[1]);
    close(hold[0]);
    MUST(read(up[0], &sandbox_init, sizeof(sandbox_init)) == sizeof(sandbox_init), "the sandbox did not start");
    close(up[0]);
    sandbox_hold = hold[1];
}

static int ns_fd(pid_t pid, const char *kind) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ns/%s", (int) pid, kind);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    MUST(fd >= 0, "cannot open %s", path);
    return fd;
}

static int same_ns(int fd, const char *self_kind) {
    char path[64];
    struct stat mine, theirs;
    snprintf(path, sizeof(path), "/proc/self/ns/%s", self_kind);
    if (stat(path, &mine) != 0 || fstat(fd, &theirs) != 0)
        return -1;
    return mine.st_ino == theirs.st_ino;
}

static void inside_checks(const char *what) {
    char host[64] = "";
    MUST(gethostname(host, sizeof(host)) == 0 && strcmp(host, "box") == 0, "%s: the host name is \"%s\"", what, host);
    MUST(geteuid() == 0, "%s: the caller's uid in the sandbox is %u", what, (unsigned) geteuid());
    pid_t me = getpid();
    int fds[2];
    MUST(pipe(fds) == 0, "pipe");
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        pid_t report[2] = {getpid(), getppid()};
        _exit(kill(1, 0) == 0 && write(fds[1], report, sizeof(report)) == sizeof(report) ? 0 : 9);
    }
    MUST(child > 0, "%s: fork into the sandbox failed", what);
    close(fds[1]);
    pid_t report[2] = {-1, -1};
    MUST(read(fds[0], report, sizeof(report)) == sizeof(report), "%s: the child did not report (or could not signal pid 1)", what);
    MUST(report[0] > 1 && report[0] < 10, "%s: the child is pid %d in the sandbox", what, (int) report[0]);
    MUST(report[1] == 0, "%s: the child's parent, who is outside, reads as %d", what, (int) report[1]);
    MUST(getpid() == me, "%s: the caller's own pid changed, %d to %d", what, (int) me, (int) getpid());
    MUST(wait_status(child) == 0, "%s: the child failed", what);
}

static void case_one_by_one(void) {
    int user = ns_fd(sandbox_init, "user"), pid = ns_fd(sandbox_init, "pid");
    int net = ns_fd(sandbox_init, "net"), uts = ns_fd(sandbox_init, "uts");
    int own_pid = ns_fd(getpid(), "pid");

    errno = 0;
    MUST(setns(pid, CLONE_NEWPID) == -1 && errno == EPERM, "joining the pid namespace before the user namespace did not fail with EPERM");
    errno = 0;
    MUST(setns(net, CLONE_NEWNET) == -1 && errno == EPERM, "joining the network namespace before the user namespace did not fail with EPERM");
    errno = 0;
    MUST(setns(uts, CLONE_NEWUTS) == -1 && errno == EPERM, "joining the UTS namespace before the user namespace did not fail with EPERM");

    MUST(setns(user, CLONE_NEWUSER) == 0, "joining the user namespace failed");
    MUST(same_ns(user, "user") == 1, "/proc/self/ns/user is not the sandbox's after joining it");
    errno = 0;
    MUST(setns(user, CLONE_NEWUSER) == -1 && errno == EINVAL, "joining the user namespace one is in did not fail with EINVAL");

    MUST(setns(uts, CLONE_NEWUTS) == 0, "joining the UTS namespace failed");
    MUST(setns(net, CLONE_NEWNET) == 0, "joining the network namespace failed");
    MUST(same_ns(net, "net") == 1, "/proc/self/ns/net is not the sandbox's after joining it");

    MUST(same_ns(pid, "pid") == 0, "the caller was in the sandbox's pid namespace before joining it");
    MUST(setns(pid, CLONE_NEWPID) == 0, "joining the pid namespace failed");
    MUST(same_ns(pid, "pid") == 0 && same_ns(own_pid, "pid") == 1, "joining a pid namespace moved the caller itself");
    MUST(same_ns(pid, "pid_for_children") == 1, "/proc/self/ns/pid_for_children is not the sandbox's after joining it");
    inside_checks("one by one");
}

static void case_pidfd(void) {
    int pidfd = (int) syscall(SYS_pidfd_open, sandbox_init, 0);
    MUST(pidfd >= 0, "pidfd_open of the sandbox's init failed");
    errno = 0;
    MUST(setns(pidfd, CLONE_NEWPID | CLONE_NEWNET) == -1 && errno == EPERM, "joining through a pidfd without the user namespace did not fail with EPERM");
    MUST(setns(pidfd, CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWUTS) == 0, "joining all four through a pidfd failed");
    int net = ns_fd(sandbox_init, "net");
    MUST(same_ns(net, "net") == 1, "/proc/self/ns/net is not the sandbox's after joining through a pidfd");
    inside_checks("pidfd");
}

// A pid namespace above one's own: from the init of a namespace made inside
// the sandbox's, back to the sandbox's.
static void case_up(void) {
    int user = ns_fd(sandbox_init, "user"), pid = ns_fd(sandbox_init, "pid");
    MUST(setns(user, CLONE_NEWUSER) == 0, "joining the user namespace failed");
    MUST(setns(pid, CLONE_NEWPID) == 0, "joining the pid namespace failed");
    fflush(stdout);
    pid_t inside = fork();
    if (inside == 0) {
        MUST(unshare(CLONE_NEWPID) == 0, "unshare(CLONE_NEWPID) in the sandbox failed");
        pid_t deeper = fork();
        if (deeper == 0) {
            errno = 0;
            MUST(setns(pid, CLONE_NEWPID) == -1 && errno == EINVAL, "joining a pid namespace above one's own did not fail with EINVAL");
            _exit(0);
        }
        _exit(wait_status(deeper) == 0 ? 0 : 9);
    }
    MUST(wait_status(inside) == 0, "the process inside failed");
}

static int same_fd_ns(int a, int b) {
    struct stat sa, sb;
    if (fstat(a, &sa) != 0 || fstat(b, &sb) != 0)
        return -1;
    return sa.st_ino == sb.st_ino;
}

static void case_ioctls(void) {
    int user = ns_fd(sandbox_init, "user"), pid = ns_fd(sandbox_init, "pid"), net = ns_fd(sandbox_init, "net");
    int own_user = ns_fd(getpid(), "user"), own_pid = ns_fd(getpid(), "pid"), own_uts = ns_fd(getpid(), "uts");

    int owner = ioctl(pid, NS_GET_USERNS);
    MUST(owner >= 0 && same_fd_ns(owner, user) == 1, "NS_GET_USERNS on the sandbox's pid namespace is not its user namespace (%d)", owner);
    owner = ioctl(net, NS_GET_USERNS);
    MUST(owner >= 0 && same_fd_ns(owner, user) == 1, "NS_GET_USERNS on the sandbox's network namespace is not its user namespace (%d)", owner);
    owner = ioctl(own_uts, NS_GET_USERNS);
    MUST(owner >= 0 && same_fd_ns(owner, own_user) == 1, "NS_GET_USERNS on the caller's UTS namespace is not its user namespace (%d)", owner);
    owner = ioctl(user, NS_GET_USERNS);
    MUST(owner >= 0 && same_fd_ns(owner, own_user) == 1, "NS_GET_USERNS on the sandbox's user namespace is not the caller's (%d)", owner);

    uid_t uid = (uid_t) -1;
    MUST(ioctl(user, NS_GET_OWNER_UID, &uid) == 0 && uid == outer_uid, "NS_GET_OWNER_UID says %u, wanted %u", (unsigned) uid, (unsigned) outer_uid);
    errno = 0;
    MUST(ioctl(pid, NS_GET_OWNER_UID, &uid) == -1 && errno == EINVAL, "NS_GET_OWNER_UID on a pid namespace did not fail with EINVAL");

    int parent = ioctl(user, NS_GET_PARENT);
    MUST(parent >= 0 && same_fd_ns(parent, own_user) == 1, "NS_GET_PARENT of the sandbox's user namespace is not the caller's (%d)", parent);
    parent = ioctl(pid, NS_GET_PARENT);
    MUST(parent >= 0 && same_fd_ns(parent, own_pid) == 1, "NS_GET_PARENT of the sandbox's pid namespace is not the caller's (%d)", parent);
    errno = 0;
    MUST(ioctl(own_user, NS_GET_PARENT) == -1 && errno == EPERM, "NS_GET_PARENT of the caller's own user namespace did not fail with EPERM");
    errno = 0;
    MUST(ioctl(own_pid, NS_GET_PARENT) == -1 && errno == EPERM, "NS_GET_PARENT of the caller's own pid namespace did not fail with EPERM");
    errno = 0;
    MUST(ioctl(net, NS_GET_PARENT) == -1 && errno == EINVAL, "NS_GET_PARENT of a network namespace did not fail with EINVAL");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (geteuid() == 0) {
        if (setgroups(0, NULL) != 0 || setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
            printf(NAME ": FAIL setup: cannot become uid 1000: %s\n", strerror(errno));
            return 1;
        }
        // (Changing ids makes a process undumpable, and its /proc/<pid>/ns
        // root's alone.)
        prctl(PR_SET_DUMPABLE, 1);
    }
    outer_uid = geteuid();
    outer_gid = getegid();
    fflush(stdout);
    pid_t probe = fork();
    if (probe == 0)
        _exit(unshare(CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWUTS) == 0 ? 0 : 8);
    int status = 0;
    waitpid(probe, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 8) {
        printf(NAME ": SKIP (an ordinary user cannot make these namespaces here)\n");
        return 0;
    }

    sandbox_start();
    run("one by one", case_one_by_one);
    run("through a pidfd", case_pidfd);
    run("a pid namespace above", case_up);
    run("the nsfs ioctls", case_ioctls);
    close(sandbox_hold);

    if (failures != 0) {
        printf(NAME ": FAIL failures=%d\n", failures);
        return 1;
    }
    printf(NAME ": PASS\n");
    return 0;
}
