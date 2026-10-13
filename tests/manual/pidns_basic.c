// Pid namespaces: a set of processes that see only each other, the first of
// them as pid 1.
//
// AOK had one pid space: clone(CLONE_NEWPID) was EINVAL and unshare ENOSYS.
// bubblewrap --unshare-pid (and so Codex CLI's command sandbox) asks for one.
// kernel/pid_ns.h has the model: a task keeps its machine pid, and each
// namespace it is in gives it a second number, counted from 1 as Linux
// counts them.
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
//   numbers      the init's children are 2, then 3 (the numbers go round,
//                they do not go back to one that was freed), a thread's tid
//                is the next; a group and session from outside read 0, and
//                kill(0) still reaches the caller's own group
//   reported     the same number wherever a pid is given back: waitid and
//                SIGCHLD's si_pid, F_GETLK's l_pid, F_GETOWN, SO_PEERCRED
//                (0 for a peer from outside), /proc/<pid>/stat and status
//   NSpid        /proc/<pid>/status lists a task's number in the reader's
//                namespace and in each one below, so the parent outside
//                reads "<pid> 1" for an init, and three numbers for the
//                init of a namespace made inside that one
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
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

static void *thread_tid(void *arg) {
    *(pid_t *) arg = (pid_t) syscall(SYS_gettid);
    return NULL;
}

static void init_numbers(void) {
    // The group and session it was born into are outside.
    MUST(getpgid(0) == 0, "a process group from outside reads %d", (int) getpgid(0));
    MUST(getsid(0) == 0, "a session from outside reads %d", (int) getsid(0));
    MUST(kill(0, 0) == 0, "kill(0, 0) did not reach the caller's own group");

    pid_t first = fork();
    if (first == 0)
        _exit(getpid() == 2 && getppid() == 1 ? 0 : 9);
    MUST(first == 2, "the init's first child is pid %d", (int) first);
    MUST(wait_status(first) == 0, "the first child did not call itself 2");

    // The next number, not the one just freed.
    int fds[2];
    MUST(pipe(fds) == 0, "pipe");
    pid_t second = fork();
    if (second == 0) {
        pid_t group = setpgid(0, 0) == 0 ? getpgid(0) : -1;
        _exit(getpid() == 3 && write(fds[1], &group, sizeof(group)) == sizeof(group) ? 0 : 9);
    }
    MUST(second == 3, "the init's second child is pid %d", (int) second);
    close(fds[1]);
    pid_t group = -1;
    MUST(read(fds[0], &group, sizeof(group)) == sizeof(group) && group == 3, "a group made inside reads %d to its leader", (int) group);
    siginfo_t exited;
    memset(&exited, 0, sizeof(exited));
    MUST(waitid(P_PID, 3, &exited, WEXITED) == 0 && exited.si_pid == 3 && exited.si_status == 0,
            "waitid for pid 3 says pid %d, status %d", (int) exited.si_pid, exited.si_status);

    pthread_t thread;
    pid_t tid = -1;
    MUST(pthread_create(&thread, NULL, thread_tid, &tid) == 0, "pthread_create");
    pthread_join(thread, NULL);
    MUST(tid == 4, "a thread of the init has tid %d", (int) tid);
}

static void case_numbers(void) {
    enter(CLONE_NEWPID);
    in_new_pidns(init_numbers, "numbers");
}

static volatile sig_atomic_t chld_pid = -1;
static void on_chld(int sig, siginfo_t *info, void *context) {
    (void) sig, (void) context;
    chld_pid = info->si_pid;
}

static int outside_socket = -1;     // one end of a socketpair made outside the namespace

// Field `want` (1-based) of /proc/<pid>/stat, counted from after the name.
static long stat_field(pid_t pid, int want) {
    char path[64], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int) pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -2;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -2;
    buf[n] = '\0';
    if (want == 1)
        return atol(buf);
    char *at = strrchr(buf, ')');
    for (int field = 2; at != NULL && field < want; field++)
        at = strchr(at + 1, ' ');
    return at != NULL ? atol(at + 1) : -2;
}

// The line of /proc/<pid>/status that starts with `key`, less the key.
static const char *status_line(pid_t pid, const char *key) {
    static char buf[8192];
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int) pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return "(unreadable)";
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return "(unreadable)";
    buf[n] = '\0';
    char *at = buf;
    while (at != NULL && strncmp(at, key, strlen(key)) != 0) {
        at = strchr(at, '\n');
        if (at != NULL)
            at++;
    }
    if (at == NULL)
        return "(absent)";
    char *end = strchr(at, '\n');
    if (end != NULL)
        *end = '\0';
    return at + strlen(key);
}

static void init_reported(void) {
    MUST(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0, "making the tree private failed");
    MUST(mount("proc", "/proc", "proc", 0, NULL) == 0, "mounting /proc in the namespace failed");
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_chld;
    action.sa_flags = SA_SIGINFO;
    MUST(sigaction(SIGCHLD, &action, NULL) == 0, "sigaction");

    char path[] = "/tmp/pidns_basic.XXXXXX";
    int file = mkstemp(path);
    MUST(file >= 0, "mkstemp");
    unlink(path);
    int locked[2], done[2];
    MUST(pipe(locked) == 0 && pipe(done) == 0, "pipe");
    pid_t child = fork();
    if (child == 0) {
        close(done[1]);
        struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1};
        char c = 0;
        if (fcntl(file, F_SETLK, &lock) != 0 || write(locked[1], &c, 1) != 1)
            _exit(9);
        (void) !read(done[0], &c, 1);
        _exit(0);
    }
    MUST(child == 2, "the child is pid %d", (int) child);
    close(done[0]);
    char c;
    MUST(read(locked[0], &c, 1) == 1, "the child did not take its lock");

    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1};
    MUST(fcntl(file, F_GETLK, &lock) == 0 && lock.l_type == F_WRLCK && lock.l_pid == 2,
            "F_GETLK names pid %d (type %d) for the child's lock", (int) lock.l_pid, (int) lock.l_type);
    MUST(fcntl(locked[0], F_SETOWN, child) == 0 && fcntl(locked[0], F_GETOWN) == 2,
            "F_GETOWN reads %d after F_SETOWN to the child", fcntl(locked[0], F_GETOWN));

    int pair[2];
    MUST(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    struct ucred cred = {.pid = -1};
    socklen_t len = sizeof(cred);
    MUST(getsockopt(pair[0], SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.pid == 1,
            "SO_PEERCRED on the init's own socketpair says pid %d", (int) cred.pid);
    cred.pid = -1;
    len = sizeof(cred);
    MUST(getsockopt(outside_socket, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.pid == 0,
            "SO_PEERCRED for a peer outside the namespace says pid %d", (int) cred.pid);

    MUST(stat_field(1, 1) == 1 && stat_field(1, 4) == 0, "/proc/1/stat: pid %ld, parent %ld", stat_field(1, 1), stat_field(1, 4));
    MUST(stat_field(1, 5) == 0 && stat_field(1, 6) == 0, "/proc/1/stat: group %ld, session %ld; both are outside", stat_field(1, 5), stat_field(1, 6));
    MUST(stat_field(2, 1) == 2 && stat_field(2, 4) == 1, "/proc/2/stat: pid %ld, parent %ld", stat_field(2, 1), stat_field(2, 4));
    MUST(strcmp(status_line(2, "Pid:"), "\t2") == 0, "/proc/2/status Pid:%s", status_line(2, "Pid:"));
    MUST(strcmp(status_line(2, "PPid:"), "\t1") == 0, "/proc/2/status PPid:%s", status_line(2, "PPid:"));
    MUST(strcmp(status_line(2, "NSpid:"), "\t2") == 0, "/proc/2/status NSpid:%s", status_line(2, "NSpid:"));
    MUST(strcmp(status_line(1, "NStgid:"), "\t1") == 0, "/proc/1/status NStgid:%s", status_line(1, "NStgid:"));
    MUST(strcmp(status_line(1, "NSpgid:"), "\t0") == 0, "/proc/1/status NSpgid:%s", status_line(1, "NSpgid:"));
    MUST(strcmp(status_line(1, "NSsid:"), "\t0") == 0, "/proc/1/status NSsid:%s", status_line(1, "NSsid:"));
    struct stat st;
    MUST(stat("/proc/2/task/2", &st) == 0, "no /proc/2/task/2");

    close(done[1]);
    for (int i = 0; i < 500 && chld_pid == -1; i++)
        usleep(10000);
    MUST(chld_pid == 2, "SIGCHLD for the child says si_pid %d", (int) chld_pid);
    MUST(wait_status(child) == 0, "the child failed");
}

static void case_reported(void) {
    int pair[2];
    MUST(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair");
    outside_socket = pair[0];
    enter(CLONE_NEWPID | CLONE_NEWNS);
    in_new_pidns(init_reported, "reported");
}

// What the parent outside is told of the namespaces below it.
static void case_nspid(void) {
    enter(CLONE_NEWPID);
    int up[2], hold[2];
    MUST(pipe(up) == 0 && pipe(hold) == 0, "pipe");
    fflush(stdout);
    pid_t init = fork();
    MUST(init >= 0, "fork");
    if (init == 0) {
        close(up[0]);
        close(hold[1]);
        // A namespace inside this one, and its init.
        MUST(unshare(CLONE_NEWPID) == 0, "unshare(CLONE_NEWPID) inside a pid namespace failed");
        pid_t inner = fork();
        if (inner == 0) {
            pid_t me = getpid();
            char c;
            if (write(up[1], &me, sizeof(me)) != sizeof(me))
                _exit(9);
            (void) !read(hold[0], &c, 1);
            _exit(0);
        }
        MUST(write(up[1], &inner, sizeof(inner)) == sizeof(inner), "write");
        close(up[1]);
        close(hold[0]);
        _exit(wait_status(inner) == 0 ? 0 : 9);
    }
    close(up[1]);
    close(hold[0]);
    pid_t first = -1, second = -1;
    MUST(read(up[0], &first, sizeof(first)) == sizeof(first) && read(up[0], &second, sizeof(second)) == sizeof(second),
            "the two inits did not report");
    // In either order: one is the inner init's own pid, the other what its
    // parent calls it.
    MUST((first == 1 && second == 2) || (first == 2 && second == 1),
            "the inner init is %d to itself and %d to its parent, or the reverse; wanted 1 and 2", (int) first, (int) second);

    char want[64];
    snprintf(want, sizeof(want), "\t%d\t1", (int) init);
    MUST(strcmp(status_line(init, "NSpid:"), want) == 0, "/proc/%d/status NSpid:%s for the init of a child namespace", (int) init, status_line(init, "NSpid:"));
    // The inner init: the one process whose NSpid has three numbers.
    int found = 0;
    DIR *dir = opendir("/proc");
    MUST(dir != NULL, "opendir /proc");
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        pid_t pid = atoi(entry->d_name);
        snprintf(want, sizeof(want), "\t%d\t2\t1", (int) pid);
        if (strcmp(status_line(pid, "NSpid:"), want) == 0 && stat_field(pid, 4) == init)
            found++;
    }
    closedir(dir);
    MUST(found == 1, "%d processes under the init read NSpid \"<pid> 2 1\"; wanted the inner init", found);
    close(hold[1]);
    int status = wait_status(init);
    MUST(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the init: status %#x", status);
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
    run("numbers", case_numbers);
    run("the numbers reported", case_reported);
    run("NSpid", case_nspid);

    if (failures != 0) {
        printf(NAME ": FAIL failures=%d\n", failures);
        return 1;
    }
    printf(NAME ": PASS\n");
    return 0;
}
