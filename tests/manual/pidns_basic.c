// Pid namespaces: a set of processes that see only each other, the first of
// them as pid 1.
//
// AOK had one pid space: clone(CLONE_NEWPID) was EINVAL and unshare ENOSYS.
// bubblewrap --unshare-pid (and so Codex CLI's command sandbox) asks for one.
// kernel/pid_ns.h has the model -- pid numbers stay the machine's, and a
// namespace is a filter on what a number may name plus "its init is 1" -- so
// the numbers of a namespace's OTHER processes are not small here as they are
// on Linux, and nothing below asks what they are.
//
// As an ordinary user in a user namespace, which is how it runs on Linux as
// the oracle (6.12, camd); as root the test becomes uid 1000 first.
//
//   unshare      the caller keeps its pid; its next child is pid 1 with
//                parent 0 (its parent is outside), tid 1, and /proc/<pid>/ns
//                shows the caller a pid_for_children that is not its pid
//   clone        clone(CLONE_NEWPID) makes the child pid 1 directly
//   outside      from inside, the parent's pid names nothing (ESRCH)
//   children     a child of the init has parent 1 and a pid that is not 1;
//                the init waits for it by that pid
//   init         kill -9 of the init from inside the namespace does nothing,
//                from the init itself too; a signal it has a handler for is
//                delivered, and its si_pid is the sender's pid as the
//                namespace has it; a signal from OUTSIDE has si_pid 0
//   orphans      a process whose parent exits becomes the init's child
//   /proc        mounted inside: /proc/self is "1", only the namespace's
//                processes are listed, and the parent's pid is not there
//   groups       setsid and getpgid in the init say 1
//   the end      when the init exits, every other process in the namespace
//                is killed, and a fork into the namespace is ENOMEM
//   refused      CLONE_NEWPID with CLONE_THREAD is EINVAL
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NAME "pidns_basic"
static int failures;
static uid_t outer_uid;
static gid_t outer_gid;

#define CHECK(cond, ...) do { if (!(cond)) { printf(NAME ": " __VA_ARGS__); printf(" (errno %d: %s)\n", errno, strerror(errno)); failures++; } } while (0)
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

// Root in a new user namespace, with `more` namespaces besides.
static void enter(int more) {
    char map[64];
    MUST(unshare(CLONE_NEWUSER | more) == 0, "unshare(CLONE_NEWUSER|%#x) failed", more);
    MUST(write_file("/proc/self/setgroups", "deny") == 0, "writing setgroups deny failed");
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) outer_uid);
    MUST(write_file("/proc/self/uid_map", map) == 0, "writing the uid map failed");
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) outer_gid);
    MUST(write_file("/proc/self/gid_map", map) == 0, "writing the gid map failed");
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

// The init of a new pid namespace runs `init_body` and exits with its
// status; the caller (outside) waits for it.
static void in_new_pidns(void (*init_body)(void), const char *what) {
    fflush(stdout);
    pid_t init = fork();
    MUST(init >= 0, "%s: fork into the new pid namespace failed", what);
    if (init == 0) {
        init_body();
        _exit(0);
    }
    int status = wait_status(init);
    MUST(WIFEXITED(status) && WEXITSTATUS(status) == 0, "%s: the namespace's init left with status %#x", what, status);
}

static pid_t outside_pid;       // the pid of the process that made the namespace, as it knows it

static void init_basics(void) {
    MUST(getpid() == 1, "the first process in a new pid namespace is pid %d", (int) getpid());
    MUST((pid_t) syscall(SYS_gettid) == 1, "its tid is %d", (int) syscall(SYS_gettid));
    MUST(getppid() == 0, "its parent, who is outside, reads as %d", (int) getppid());
    errno = 0;
    MUST(kill(outside_pid, 0) == -1 && errno == ESRCH, "the parent's pid %d names something from inside", (int) outside_pid);
    MUST(kill(1, 0) == 0, "pid 1 does not name the init to itself");
    MUST(setsid() == 1 || errno == EPERM, "setsid in the init returned something that is not 1");
    MUST(getpgid(0) == 1 || getpgid(0) == 0, "the init's process group reads %d", (int) getpgid(0));

    // A child of the init.
    int fds[2];
    MUST(pipe(fds) == 0, "pipe");
    pid_t child = fork();
    if (child == 0) {
        pid_t me = getpid();
        _exit(getppid() == 1 && me != 1 && me > 0 && write(fds[1], &me, sizeof(me)) == sizeof(me) ? 0 : 9);
    }
    MUST(child > 1, "the init's child is pid %d to it", (int) child);
    close(fds[1]);
    pid_t told = 0;
    MUST(read(fds[0], &told, sizeof(told)) == sizeof(told) && told == child, "the child calls itself %d, its parent calls it %d", (int) told, (int) child);
    int status = wait_status(child);
    MUST(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the init's child: status %#x (its parent was not 1, or its pid was)", status);
}

static void case_unshare(void) {
    char mine[64], for_children[64];
    pid_t before = getpid();
    enter(CLONE_NEWPID);
    MUST(getpid() == before, "unshare(CLONE_NEWPID) changed the caller's own pid from %d to %d", (int) before, (int) getpid());
    // (Linux gives the link no target until the namespace has its init.)
    ssize_t a = readlink("/proc/self/ns/pid", mine, sizeof(mine) - 1);
    ssize_t b = readlink("/proc/self/ns/pid_for_children", for_children, sizeof(for_children) - 1);
    MUST(a > 0, "no /proc/self/ns/pid");
    mine[a] = '\0';
    for_children[b > 0 ? b : 0] = '\0';
    MUST(strcmp(mine, for_children) != 0, "after the unshare pid_for_children is still %s", mine);
    outside_pid = before;
    in_new_pidns(init_basics, "unshare");
}

static int cloned_init(void *arg) {
    (void) arg;
    return getpid() == 1 && getppid() == 0 ? 0 : 5;
}

static void case_clone(void) {
    static char stack[65536];
    enter(0);
    pid_t pid = clone(cloned_init, stack + sizeof(stack), CLONE_NEWPID | SIGCHLD, NULL);
    MUST(pid > 1, "clone(CLONE_NEWPID) returned %d", (int) pid);
    int status = wait_status(pid);
    MUST(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the cloned init: status %#x", status);
    errno = 0;
    pid = clone(cloned_init, stack + sizeof(stack), CLONE_NEWPID | CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, NULL);
    MUST(pid == -1 && errno == EINVAL, "clone(CLONE_NEWPID|CLONE_THREAD) did not fail with EINVAL (%d)", (int) pid);
}

static volatile sig_atomic_t got_usr1, usr1_sender = -1;
static void on_usr1(int sig, siginfo_t *info, void *context) {
    (void) sig;
    (void) context;
    usr1_sender = info->si_pid;
    got_usr1++;
}

static void init_signals(void) {
    struct sigaction sa = {.sa_sigaction = on_usr1, .sa_flags = SA_SIGINFO | SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    // From itself: nothing.
    MUST(kill(1, SIGKILL) == 0, "the init's kill(1, SIGKILL) failed");
    MUST(kill(1, SIGTERM) == 0, "the init's kill(1, SIGTERM) failed");

    int fds[2];
    MUST(pipe(fds) == 0, "pipe");
    pid_t child = fork();
    if (child == 0) {
        pid_t me = getpid();
        // SIGKILL and an unhandled SIGTERM from inside do nothing to the
        // init; SIGUSR1, which it handles, arrives.
        if (kill(1, SIGKILL) != 0 || kill(1, SIGTERM) != 0 || kill(1, SIGUSR1) != 0)
            _exit(6);
        _exit(write(fds[1], &me, sizeof(me)) == sizeof(me) ? 0 : 7);
    }
    close(fds[1]);
    pid_t told = 0;
    MUST(read(fds[0], &told, sizeof(told)) == sizeof(told), "the child did not report");
    int status = wait_status(child);
    MUST(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the signalling child: status %#x", status);
    for (int i = 0; i < 200 && got_usr1 == 0; i++)
        usleep(10000);
    MUST(got_usr1 == 1, "the init took %d SIGUSR1s from its child, wanted 1", (int) got_usr1);
    MUST(usr1_sender == told, "the SIGUSR1 from inside says its sender is %d; the sender is %d", (int) usr1_sender, (int) told);

    // Now one from outside: tell the parent we are ready, and wait for it.
    got_usr1 = 0;
    usr1_sender = -1;
    MUST(write(3, "r", 1) == 1, "cannot tell the parent");
    for (int i = 0; i < 500 && got_usr1 == 0; i++)
        usleep(10000);
    MUST(got_usr1 == 1, "the init did not get the SIGUSR1 from outside");
    MUST(usr1_sender == 0, "a SIGUSR1 from outside the namespace says its sender is %d, wanted 0", (int) usr1_sender);
}

static void case_init_signals(void) {
    enter(CLONE_NEWPID);
    int ready[2];
    MUST(pipe(ready) == 0, "pipe");
    fflush(stdout);
    pid_t init = fork();
    MUST(init >= 0, "fork");
    if (init == 0) {
        dup2(ready[1], 3);
        init_signals();
        _exit(0);
    }
    close(ready[1]);
    char c;
    MUST(read(ready[0], &c, 1) == 1, "the init did not get as far as asking for the outside signal");
    MUST(kill(init, SIGUSR1) == 0, "kill of the init from outside failed");
    int status = wait_status(init);
    MUST(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the init: status %#x", status);
}

static void init_orphans(void) {
    int fds[2];
    MUST(pipe(fds) == 0, "pipe");
    pid_t middle = fork();
    if (middle == 0) {
        pid_t orphan = fork();
        if (orphan == 0) {
            // Wait to be orphaned, then say who the parent is.
            for (int i = 0; i < 500 && getppid() != 1; i++)
                usleep(10000);
            pid_t parent = getppid();
            _exit(write(fds[1], &parent, sizeof(parent)) == sizeof(parent) ? 0 : 8);
        }
        _exit(0);
    }
    MUST(wait_status(middle) == 0, "the middle process did not exit 0");
    close(fds[1]);
    pid_t parent = -1;
    MUST(read(fds[0], &parent, sizeof(parent)) == sizeof(parent), "the orphan did not report");
    MUST(parent == 1, "an orphan's parent is %d, wanted the namespace's init", (int) parent);
    // ...and it is the init's to reap.
    int status = -1;
    pid_t reaped = wait(&status);
    MUST(reaped > 1 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "the init did not reap the orphan (%d, %#x)", (int) reaped, status);
}

static void case_orphans(void) {
    enter(CLONE_NEWPID);
    in_new_pidns(init_orphans, "orphans");
}

static void init_proc(void) {
    MUST(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0, "making the tree private failed");
    MUST(mount("proc", "/proc", "proc", 0, NULL) == 0, "mounting /proc in the namespace failed");
    char link[64];
    ssize_t n = readlink("/proc/self", link, sizeof(link) - 1);
    MUST(n > 0, "no /proc/self");
    link[n] = '\0';
    MUST(strcmp(link, "1") == 0, "/proc/self is \"%s\" for the init", link);
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d", (int) outside_pid);
    struct stat st;
    errno = 0;
    MUST(stat(path, &st) == -1 && errno == ENOENT, "%s, the parent outside, is there", path);
    pid_t child = fork();
    if (child == 0) {
        pause();
        _exit(0);
    }
    int listed = 0, saw_init = 0, saw_child = 0;
    DIR *dir = opendir("/proc");
    MUST(dir != NULL, "opendir /proc");
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        listed++;
        if (atoi(entry->d_name) == 1)
            saw_init = 1;
        if (atoi(entry->d_name) == child)
            saw_child = 1;
    }
    closedir(dir);
    MUST(listed == 2 && saw_init && saw_child, "/proc lists %d processes (init %d, child %d); wanted the two in the namespace", listed, saw_init, saw_child);
    kill(child, SIGKILL);
    wait_status(child);
}

static void case_proc(void) {
    outside_pid = getpid();
    enter(CLONE_NEWPID | CLONE_NEWNS);
    in_new_pidns(init_proc, "/proc");
}

static void case_the_end(void) {
    enter(CLONE_NEWPID);
    int alive[2];
    MUST(pipe(alive) == 0, "pipe");
    fflush(stdout);
    pid_t init = fork();
    MUST(init >= 0, "fork");
    if (init == 0) {
        close(alive[0]);
        pid_t straggler = fork();
        if (straggler == 0) {
            // Holds the pipe open for as long as it lives.
            sleep(60);
            _exit(0);
        }
        _exit(straggler > 1 ? 0 : 9);
    }
    close(alive[1]);
    int status = wait_status(init);
    MUST(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the init: status %#x", status);
    // The straggler's end of the pipe closes when it is killed: well before
    // its sleep ends.
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    char c;
    ssize_t n = read(alive[0], &c, 1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    MUST(n == 0 && t1.tv_sec - t0.tv_sec < 20, "a process left in the namespace outlived its init (read %zd after %ld s)", n, (long) (t1.tv_sec - t0.tv_sec));
    errno = 0;
    pid_t late = fork();
    if (late == 0)
        _exit(0);
    MUST(late == -1 && errno == ENOMEM, "a fork into a namespace whose init is gone did not fail with ENOMEM (%d)", (int) late);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (geteuid() == 0) {
        if (setgroups(0, NULL) != 0 || setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
            printf(NAME ": FAIL setup: cannot become uid 1000: %s\n", strerror(errno));
            return 1;
        }
    }
    outer_uid = geteuid();
    outer_gid = getegid();
    fflush(stdout);
    pid_t probe = fork();
    if (probe == 0)
        _exit(unshare(CLONE_NEWUSER | CLONE_NEWPID) == 0 ? 0 : 8);
    int status = 0;
    waitpid(probe, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 8) {
        // (Told apart from a kernel with no pid namespaces only by the
        // message: both are "cannot be run here".)
        printf(NAME ": SKIP (an ordinary user cannot make a pid namespace here)\n");
        return 0;
    }

    run("unshare", case_unshare);
    run("clone", case_clone);
    run("signals to the init", case_init_signals);
    run("orphans", case_orphans);
    run("/proc", case_proc);
    run("the end of a namespace", case_the_end);

    if (failures != 0) {
        printf(NAME ": FAIL failures=%d\n", failures);
        return 1;
    }
    printf(NAME ": PASS\n");
    return 0;
}
