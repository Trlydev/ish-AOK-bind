// User namespaces: a process that is root in a world of its own, and nobody
// new in the real one.
//
// AOK had none: clone(CLONE_NEWUSER) was EINVAL and unshare ENOSYS, which is
// the first thing bubblewrap asks for (and so the first thing Codex CLI's
// command sandbox dies on). kernel/user_ns.h has the model. What is checked
// here, as an ordinary user -- as root the test becomes uid 1000 first, since
// what matters is what a user namespace gives someone who had nothing:
//
//   unshare      succeeds with no privilege; before a map is written the
//                process is the overflow id (65534) to itself and its maps
//                are empty; /proc/self/ns/user names a new namespace
//   maps         setgroups "deny", then "0 <uid> 1" and "0 <gid> 1": the
//                process is uid 0, gid 0 and holds every capability. A map
//                is written once (EPERM again); a gid map before setgroups is
//                denied is EPERM; an id that is not the writer's own, or
//                more than one id, is EPERM; setgroups() is EPERM once denied
//   parent       the parent writes its child's maps through /proc/<pid>/
//   ids          files of the mapped user are root's, everyone else's are
//                nobody's (65534) -- / is; chown to an unmapped id is EINVAL
//                and to a mapped one works; setuid to an unmapped id is
//                EINVAL
//   not root     /etc/shadow still does not open, sethostname and mount are
//                EPERM in the namespaces it did not make, and a device node
//                cannot be made
//   its own      with CLONE_NEWUTS the hostname is its to set, and the
//                parent's does not change; with CLONE_NEWNS it may mount a
//                tmpfs, files there are its own, and a mount it inherited
//                cannot be unmounted to see beneath it (EINVAL)
//   nested       a user namespace inside the user namespace
//   chroot       the namespace's root may chroot, and a chrooted process
//                can make no user namespace (EPERM)
//   file caps    the namespace's root may give a file of its own
//                capabilities; they are stored for that root (a v3 naming
//                it, which reads back inside as the v2 written), an exec by
//                a user below that root gets them, and an exec outside the
//                namespace gets nothing
//   SysV IPC     in an IPC namespace of its own, an object's ids read as the
//                namespace's (0), IPC_SET takes them the same way and an id
//                with no mapping is EINVAL
//   refused      CLONE_NEWUSER|CLONE_FS is EINVAL; so is unshare from a
//                process with more than one thread
//
// Passes on Linux 6.12 as uid 1000 (camd), where unprivileged user namespaces
// are on.
//
// And one case that only root can run, so it has had no oracle run: the
// machine's own root, in a user namespace that maps it to itself, is not the
// machine's root there. Its capabilities are the namespace's, so what the
// machine guards -- mounting in the mount namespace it came from, the
// hostname, device nodes, a sysctl -- is refused, though every id still reads
// 0. AOK decided privilege by "euid is 0" for years; this is the case that
// would let a sandboxed root straight back out if anything still did.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#define NAME "userns_basic"
static int failures;
static uid_t outer_uid;
static gid_t outer_gid;
static char scratch[64];

#define CHECK(cond, ...) do { if (!(cond)) { printf(NAME ": " __VA_ARGS__); printf(" (errno %d: %s)\n", errno, strerror(errno)); failures++; } } while (0)
// In a child: say what went wrong and leave with a status that is not 0.
#define MUST(cond, ...) do { if (!(cond)) { printf(NAME ": " __VA_ARGS__); printf(" (errno %d: %s)\n", errno, strerror(errno)); fflush(stdout); _exit(1 + __LINE__ % 200); } } while (0)

// 0, or the errno of the write.
static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return errno;
    ssize_t n = write(fd, text, strlen(text));
    int err = n < 0 ? errno : 0;
    close(fd);
    return err;
}

static int read_file(const char *path, char *out, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, out, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    out[n] = '\0';
    return (int) n;
}

static unsigned long long cap_effective(void) {
    char status[4096];
    if (read_file("/proc/self/status", status, sizeof(status)) < 0)
        return 0;
    char *line = strstr(status, "CapEff:");
    return line != NULL ? strtoull(line + 7, NULL, 16) : 0;
}

// Become root in a new user namespace (plus `more` namespaces), the way an
// unprivileged process does it for itself.
static void enter(int more) {
    char map[64];
    MUST(unshare(CLONE_NEWUSER | more) == 0, "unshare(CLONE_NEWUSER|%#x) failed", more);
    MUST(write_file("/proc/self/setgroups", "deny") == 0, "writing setgroups deny failed");
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) outer_uid);
    MUST(write_file("/proc/self/uid_map", map) == 0, "writing the uid map failed");
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) outer_gid);
    MUST(write_file("/proc/self/gid_map", map) == 0, "writing the gid map failed");
}

static void run(const char *what, void (*body)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        body();
        _exit(0);
    }
    int status = -1;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf(NAME ": %s: child status %#x\n", what, status);
        failures++;
    }
}

static void case_unshare(void) {
    char before[64], after[64], map[256];
    ssize_t n = readlink("/proc/self/ns/user", before, sizeof(before) - 1);
    MUST(n > 0, "no /proc/self/ns/user");
    before[n] = '\0';
    MUST(unshare(CLONE_NEWUSER) == 0, "unshare(CLONE_NEWUSER) failed for an ordinary user");
    n = readlink("/proc/self/ns/user", after, sizeof(after) - 1);
    MUST(n > 0, "no /proc/self/ns/user after the unshare");
    after[n] = '\0';
    MUST(strcmp(before, after) != 0, "ns/user is still %s after the unshare", before);
    MUST(getuid() == 65534 && geteuid() == 65534 && getgid() == 65534,
         "with no map the ids are %u/%u/%u, wanted the overflow id", (unsigned) getuid(), (unsigned) geteuid(), (unsigned) getgid());
    MUST(read_file("/proc/self/uid_map", map, sizeof(map)) == 0, "the uid map is not empty before it is written: \"%s\"", map);
    MUST(read_file("/proc/self/setgroups", map, sizeof(map)) > 0 && strcmp(map, "allow\n") == 0,
         "setgroups reads \"%s\" in a new namespace, wanted allow", map);
    // The capabilities are there until an exec with no root to be.
    MUST(cap_effective() != 0, "a new user namespace's creator holds no capability in it");
}

static void case_maps(void) {
    char map[64], text[256];
    MUST(unshare(CLONE_NEWUSER) == 0, "unshare failed");
    // A group map needs setgroups denied first, when the writer has nothing
    // but its own gid to offer.
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) outer_gid);
    int e = write_file("/proc/self/gid_map", map);
    MUST(e == EPERM, "a gid map with setgroups still allowed: errno %d, wanted EPERM", e);
    // Not somebody else's id, and not two of them.
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) outer_uid + 1);
    e = write_file("/proc/self/uid_map", map);
    MUST(e == EPERM, "a uid map of an id that is not the writer's: errno %d, wanted EPERM", e);
    snprintf(map, sizeof(map), "0 %u 2", (unsigned) outer_uid);
    e = write_file("/proc/self/uid_map", map);
    MUST(e == EPERM, "a uid map of two ids: errno %d, wanted EPERM", e);
    e = write_file("/proc/self/uid_map", "0 nothing 1");
    MUST(e == EINVAL, "a uid map that does not parse: errno %d, wanted EINVAL", e);

    MUST(write_file("/proc/self/setgroups", "deny") == 0, "writing setgroups deny failed");
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) outer_uid);
    MUST(write_file("/proc/self/uid_map", map) == 0, "writing the uid map failed");
    e = write_file("/proc/self/uid_map", map);
    MUST(e == EPERM, "a second write of the uid map: errno %d, wanted EPERM", e);
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) outer_gid);
    MUST(write_file("/proc/self/gid_map", map) == 0, "writing the gid map failed");
    e = write_file("/proc/self/setgroups", "allow");
    MUST(e == EPERM, "setgroups back to allow: errno %d, wanted EPERM", e);

    MUST(getuid() == 0 && geteuid() == 0 && getgid() == 0 && getegid() == 0,
         "after the maps the ids are %u/%u/%u/%u, wanted 0", (unsigned) getuid(), (unsigned) geteuid(),
         (unsigned) getgid(), (unsigned) getegid());
    uid_t r, ef, s;
    MUST(getresuid(&r, &ef, &s) == 0 && r == 0 && ef == 0 && s == 0, "getresuid is %u/%u/%u", (unsigned) r, (unsigned) ef, (unsigned) s);
    unsigned first, lower, count;
    MUST(read_file("/proc/self/uid_map", text, sizeof(text)) > 0 &&
         sscanf(text, "%u %u %u", &first, &lower, &count) == 3 && first == 0 && lower == outer_uid && count == 1,
         "the uid map reads \"%s\"", text);
    MUST((cap_effective() & 0x1fffff) == 0x1fffff, "CapEff is %#llx, wanted every capability", cap_effective());
    errno = 0;
    MUST(setgroups(0, NULL) == -1 && errno == EPERM, "setgroups() where it was denied did not fail with EPERM");
}

static void case_parent_writes(void) {
    int to_child[2], to_parent[2];
    MUST(pipe(to_child) == 0 && pipe(to_parent) == 0, "pipe");
    pid_t pid = fork();
    if (pid == 0) {
        char c;
        close(to_child[1]);
        close(to_parent[0]);
        if (unshare(CLONE_NEWUSER) != 0)
            _exit(2);
        if (write(to_parent[1], "u", 1) != 1 || read(to_child[0], &c, 1) != 1)
            _exit(3);
        _exit(getuid() == 0 && getgid() == 0 ? 0 : 4);
    }
    close(to_child[0]);
    close(to_parent[1]);
    char c, path[64], map[64];
    MUST(read(to_parent[0], &c, 1) == 1, "the child did not unshare");
    snprintf(path, sizeof(path), "/proc/%d/setgroups", (int) pid);
    MUST(write_file(path, "deny") == 0, "the parent could not deny setgroups for its child");
    snprintf(path, sizeof(path), "/proc/%d/uid_map", (int) pid);
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) outer_uid);
    MUST(write_file(path, map) == 0, "the parent could not write its child's uid map");
    snprintf(path, sizeof(path), "/proc/%d/gid_map", (int) pid);
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) outer_gid);
    MUST(write_file(path, map) == 0, "the parent could not write its child's gid map");
    MUST(write(to_child[1], "g", 1) == 1, "write to the child");
    int status = -1;
    waitpid(pid, &status, 0);
    MUST(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child the parent mapped: status %#x", status);
}

static void case_ids(void) {
    char mine[128];
    snprintf(mine, sizeof(mine), "%s/mine", scratch);
    int fd = open(mine, O_WRONLY | O_CREAT, 0600);
    MUST(fd >= 0, "cannot create %s", mine);
    close(fd);
    enter(0);
    struct stat st;
    MUST(stat(mine, &st) == 0 && st.st_uid == 0 && st.st_gid == 0, "the mapped user's file is %u:%u, wanted 0:0",
         (unsigned) st.st_uid, (unsigned) st.st_gid);
    MUST(stat("/", &st) == 0 && st.st_uid == 65534 && st.st_gid == 65534, "/ is %u:%u, wanted nobody's (65534)",
         (unsigned) st.st_uid, (unsigned) st.st_gid);
    errno = 0;
    MUST(chown(mine, 1, 1) == -1 && errno == EINVAL, "chown to an id the namespace does not map did not fail with EINVAL");
    MUST(chown(mine, 0, 0) == 0, "chown to the id the namespace maps failed");
    errno = 0;
    MUST(setuid(1) == -1 && errno == EINVAL, "setuid to an unmapped id did not fail with EINVAL");
    MUST(setresuid(0, 0, 0) == 0 && setresgid(0, 0, 0) == 0, "setresuid/setresgid to the mapped id failed");
    // A file only its owner may not read: the namespace's root may.
    MUST(chmod(mine, 0) == 0, "chmod 0 failed");
    fd = open(mine, O_RDONLY);
    MUST(fd >= 0, "the namespace's root cannot open its own mode-0 file");
    close(fd);
}

static void case_not_root(void) {
    enter(0);
    errno = 0;
    int fd = open("/etc/shadow", O_RDONLY);
    MUST(fd == -1 && (errno == EACCES || errno == ENOENT), "/etc/shadow opened for the root of a user namespace");
    errno = 0;
    MUST(sethostname("userns-test", 11) == -1 && errno == EPERM, "sethostname in a UTS namespace it did not make did not fail with EPERM");
    errno = 0;
    MUST(mount("none", scratch, "tmpfs", 0, NULL) == -1 && errno == EPERM, "mount in a mount namespace it did not make did not fail with EPERM");
    char node[128];
    snprintf(node, sizeof(node), "%s/node", scratch);
    errno = 0;
    MUST(mknod(node, S_IFCHR | 0600, makedev(1, 3)) == -1 && errno == EPERM, "mknod of a device did not fail with EPERM");
    errno = 0;
    MUST(chown("/", 0, 0) == -1 && errno == EPERM, "chown of a file the namespace does not map did not fail with EPERM");
}

static void case_own_uts(void) {
    char name[128] = "";
    enter(CLONE_NEWUTS);
    MUST(sethostname("userns-test", 11) == 0, "sethostname in its own UTS namespace failed");
    MUST(gethostname(name, sizeof(name)) == 0 && strcmp(name, "userns-test") == 0, "the hostname reads \"%s\"", name);
}

static void case_own_mounts(void) {
    char path[128];
    enter(CLONE_NEWNS);
    MUST(mount("none", scratch, "tmpfs", 0, NULL) == 0, "mount of a tmpfs in its own mount namespace failed");
    snprintf(path, sizeof(path), "%s/inside", scratch);
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    MUST(fd >= 0, "cannot create a file on its tmpfs");
    close(fd);
    struct stat st;
    MUST(stat(path, &st) == 0 && st.st_uid == 0, "a file it made on its tmpfs is uid %u", (unsigned) st.st_uid);
    MUST(umount(scratch) == 0, "unmounting its own tmpfs failed");
    // What it inherited stays where it is.
    errno = 0;
    MUST(umount2("/proc", MNT_DETACH) == -1 && errno == EINVAL, "unmounting an inherited /proc did not fail with EINVAL");
    MUST(mount(scratch, scratch, NULL, MS_BIND, NULL) == 0, "a bind in its own mount namespace failed");
}

static void case_nested(void) {
    enter(0);
    MUST(unshare(CLONE_NEWUSER) == 0, "a user namespace inside a user namespace failed");
    MUST(getuid() == 65534, "the inner namespace starts as uid %u", (unsigned) getuid());
    // setgroups stays denied below where it was denied, so the inner gid map
    // needs nothing more (bubblewrap relies on it for --uid).
    char text[32];
    MUST(read_file("/proc/self/setgroups", text, sizeof(text)) > 0 && strcmp(text, "deny\n") == 0,
         "setgroups reads \"%s\" in a namespace made where it was denied", text);
    MUST(write_file("/proc/self/uid_map", "0 0 1") == 0, "the inner uid map failed");
    MUST(write_file("/proc/self/gid_map", "0 0 1") == 0, "the inner gid map failed");
    MUST(getuid() == 0 && getgid() == 0, "the inner namespace's ids are %u/%u", (unsigned) getuid(), (unsigned) getgid());
}

static void case_chroot(void) {
    char path[160];
    enter(0);
    snprintf(path, sizeof(path), "%s/inside-root", scratch);
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    MUST(fd >= 0, "cannot create a file in the scratch directory");
    close(fd);
    MUST(chroot(scratch) == 0, "chroot by the root of a user namespace failed");
    MUST(chdir("/") == 0, "chdir to the new root failed");
    struct stat st;
    MUST(stat("/inside-root", &st) == 0, "the new root is not the directory asked for");
    errno = 0;
    MUST(unshare(CLONE_NEWUSER) == -1 && errno == EPERM, "a chrooted process made a user namespace");
}

static char self_exe[256];

// What `program --capeff` prints: the effective capabilities it was started
// with, or -1.
static long long run_capeff(const char *program) {
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        execl(program, program, "--capeff", (char *) NULL);
        _exit(127);
    }
    close(fds[1]);
    char text[64] = "";
    ssize_t n = read(fds[0], text, sizeof(text) - 1);
    close(fds[0]);
    int status = -1;
    waitpid(pid, &status, 0);
    if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    return strtoll(text, NULL, 16);
}

static int copy_file(const char *from, const char *to) {
    char buf[65536];
    int in = open(from, O_RDONLY), out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (in < 0 || out < 0)
        return -1;
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t) n) != n)
            return -1;
    close(in);
    return close(out) == 0 && n == 0 ? 0 : -1;
}

struct caps_v3 {
    uint32_t magic_etc;
    struct { uint32_t permitted, inheritable; } data[2];
    uint32_t rootid;
};
#define CAPS_V2_EFFECTIVE 0x02000001u
#define CAPS_V3_EFFECTIVE 0x03000001u
#define CAP_NET_RAW_BIT (1u << 13)

static void case_file_caps(void) {
    char with[160], without[160];
    snprintf(with, sizeof(with), "%s/with-caps", scratch);
    snprintf(without, sizeof(without), "%s/without-caps", scratch);
    MUST(copy_file(self_exe, with) == 0 && copy_file(self_exe, without) == 0, "cannot copy the test program to %s", scratch);
    MUST(run_capeff(without) == 0, "an ordinary user's exec starts with capabilities %llx", run_capeff(without));

    fflush(stdout);
    pid_t inside = fork();
    if (inside == 0) {
        enter(0);
        struct caps_v3 caps = {.magic_etc = CAPS_V2_EFFECTIVE, .data = {{CAP_NET_RAW_BIT, 0}, {0, 0}}};
        MUST(setxattr(with, "security.capability", &caps, 20, 0) == 0, "the namespace's root cannot give its own file capabilities");
        struct caps_v3 back = {0};
        ssize_t n = getxattr(with, "security.capability", &back, sizeof(back));
        MUST(n == 20 && back.magic_etc == CAPS_V2_EFFECTIVE && back.data[0].permitted == CAP_NET_RAW_BIT,
             "inside, the attribute reads back as %zd bytes, magic %#x, permitted %#x", n, back.magic_etc, back.data[0].permitted);
        // A user who is not root, in a namespace below: uid 1000 there is
        // this namespace's root.
        MUST(unshare(CLONE_NEWUSER) == 0, "a user namespace inside the user namespace failed");
        MUST(write_file("/proc/self/uid_map", "1000 0 1") == 0, "the inner uid map failed");
        MUST(write_file("/proc/self/gid_map", "1000 0 1") == 0, "the inner gid map failed");
        MUST(getuid() == 1000, "the inner namespace's uid is %u", (unsigned) getuid());
        MUST(run_capeff(without) == 0, "an exec by a user who is not root, of a file with no capabilities, starts with %llx", run_capeff(without));
        MUST(run_capeff(with) == CAP_NET_RAW_BIT, "an exec of the file with capabilities starts with %llx, wanted %x", run_capeff(with), CAP_NET_RAW_BIT);
        _exit(0);
    }
    int status = -1;
    MUST(waitpid(inside, &status, 0) == inside && WIFEXITED(status) && WEXITSTATUS(status) == 0, "inside the namespace: status %#x", status);

    // Outside: stored for the namespace's root, and worth nothing here.
    struct caps_v3 stored = {0};
    ssize_t n = getxattr(with, "security.capability", &stored, sizeof(stored));
    MUST(n == 24 && stored.magic_etc == CAPS_V3_EFFECTIVE && stored.rootid == outer_uid && stored.data[0].permitted == CAP_NET_RAW_BIT,
         "outside, the attribute reads back as %zd bytes, magic %#x, root %u", n, stored.magic_etc, (unsigned) stored.rootid);
    MUST(run_capeff(with) == 0, "an exec outside the namespace took %llx from capabilities written inside it", run_capeff(with));
    unlink(with);
    unlink(without);
}

static void case_sysv(void) {
    enter(CLONE_NEWIPC);
    int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    MUST(id >= 0, "shmget in its own IPC namespace failed");
    struct shmid_ds ds;
    memset(&ds, 0xff, sizeof(ds));
    MUST(shmctl(id, IPC_STAT, &ds) == 0, "IPC_STAT failed");
    MUST(ds.shm_perm.uid == 0 && ds.shm_perm.gid == 0 && ds.shm_perm.cuid == 0 && ds.shm_perm.cgid == 0,
         "the segment's ids read %u/%u (creator %u/%u), wanted the namespace's 0",
         (unsigned) ds.shm_perm.uid, (unsigned) ds.shm_perm.gid, (unsigned) ds.shm_perm.cuid, (unsigned) ds.shm_perm.cgid);
    MUST(ds.shm_cpid == getpid(), "the segment's creator pid reads %d", (int) ds.shm_cpid);
    ds.shm_perm.mode = 0640;
    MUST(shmctl(id, IPC_SET, &ds) == 0, "IPC_SET with the namespace's own ids failed");
    ds.shm_perm.uid = 1;
    errno = 0;
    MUST(shmctl(id, IPC_SET, &ds) == -1 && errno == EINVAL, "IPC_SET to an id with no mapping did not fail with EINVAL");
    MUST(shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_perm.uid == 0 && (ds.shm_perm.mode & 0777) == 0640,
         "after IPC_SET the segment is uid %u mode %o", (unsigned) ds.shm_perm.uid, (unsigned) ds.shm_perm.mode & 0777);
    MUST(shmctl(id, IPC_RMID, NULL) == 0, "IPC_RMID failed");
}

static void case_machine_root(void) {
    MUST(unshare(CLONE_NEWUSER) == 0, "unshare(CLONE_NEWUSER) as root failed");
    MUST(write_file("/proc/self/setgroups", "deny") == 0, "setgroups deny failed");
    MUST(write_file("/proc/self/uid_map", "0 0 1") == 0, "the uid map failed");
    MUST(write_file("/proc/self/gid_map", "0 0 1") == 0, "the gid map failed");
    MUST(getuid() == 0 && geteuid() == 0, "root mapped to itself is uid %u", (unsigned) getuid());
    errno = 0;
    MUST(mount("none", scratch, "tmpfs", 0, NULL) == -1 && errno == EPERM, "the machine's root in a user namespace mounted in the machine's mount namespace");
    errno = 0;
    MUST(sethostname("userns-test", 11) == -1 && errno == EPERM, "...set the machine's hostname");
    char node[128];
    snprintf(node, sizeof(node), "%s/node", scratch);
    errno = 0;
    MUST(mknod(node, S_IFCHR | 0600, makedev(1, 3)) == -1 && errno == EPERM, "...made a device node");
    int e = write_file("/proc/sys/kernel/hostname", "userns-test");
    MUST(e == EPERM || e == EACCES, "...wrote /proc/sys/kernel/hostname (errno %d)", e);
    errno = 0;
    MUST(setgroups(0, NULL) == -1 && errno == EPERM, "...called setgroups where it had denied it");
}

static int clone_fn(void *arg) {
    (void) arg;
    return 0;
}

static void *idle_thread(void *arg) {
    (void) arg;
    pause();
    return NULL;
}

static void case_refused(void) {
    static char stack[65536];
    errno = 0;
    int pid = clone(clone_fn, stack + sizeof(stack), CLONE_NEWUSER | CLONE_FS | SIGCHLD, NULL);
    MUST(pid == -1 && errno == EINVAL, "clone(CLONE_NEWUSER|CLONE_FS) did not fail with EINVAL (pid %d)", pid);
    pthread_t thread;
    MUST(pthread_create(&thread, NULL, idle_thread, NULL) == 0, "pthread_create");
    errno = 0;
    MUST(unshare(CLONE_NEWUSER) == -1 && errno == EINVAL, "unshare(CLONE_NEWUSER) with a second thread did not fail with EINVAL");
}

static void parent_hostname_unchanged(void) {
    char name[128] = "";
    gethostname(name, sizeof(name));
    CHECK(strcmp(name, "userns-test") != 0, "the hostname set inside a namespace is the machine's now");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--capeff") == 0) {
        printf("%llx\n", cap_effective());
        return 0;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    ssize_t exe_len = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
    if (exe_len <= 0) {
        printf(NAME ": FAIL setup: readlink /proc/self/exe: %s\n", strerror(errno));
        return 1;
    }
    self_exe[exe_len] = '\0';
    bool was_root = geteuid() == 0;
    if (was_root) {
        snprintf(scratch, sizeof(scratch), "/tmp/userns_root.XXXXXX");
        if (mkdtemp(scratch) == NULL) {
            printf(NAME ": FAIL setup: mkdtemp: %s\n", strerror(errno));
            return 1;
        }
        run("the machine's root", case_machine_root);
        rmdir(scratch);
        // As an ordinary user: that is who a user namespace is for.
        if (setgroups(0, NULL) != 0 || setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
            printf(NAME ": FAIL setup: cannot become uid 1000: %s\n", strerror(errno));
            return 1;
        }
    }
    outer_uid = geteuid();
    outer_gid = getegid();
    // Does this kernel let an ordinary user have one?
    fflush(stdout);
    pid_t probe = fork();
    if (probe == 0)
        _exit(unshare(CLONE_NEWUSER) == 0 ? 0 : errno == ENOSYS || errno == EINVAL ? 7 : 8);
    int status = 0;
    waitpid(probe, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 8) {
        printf(NAME ": SKIP (unprivileged user namespaces are turned off here)\n");
        return 0;
    }
    snprintf(scratch, sizeof(scratch), "/tmp/userns_basic.XXXXXX");
    if (mkdtemp(scratch) == NULL) {
        printf(NAME ": FAIL setup: mkdtemp: %s\n", strerror(errno));
        return 1;
    }

    run("unshare", case_unshare);
    run("maps", case_maps);
    run("parent writes the maps", case_parent_writes);
    run("ids", case_ids);
    run("not root", case_not_root);
    run("its own UTS namespace", case_own_uts);
    parent_hostname_unchanged();
    run("its own mount namespace", case_own_mounts);
    run("nested", case_nested);
    run("chroot", case_chroot);
    run("SysV IPC", case_sysv);
    run("file capabilities", case_file_caps);
    run("refused", case_refused);

    char path[128];
    snprintf(path, sizeof(path), "%s/mine", scratch);
    chmod(path, 0600);
    unlink(path);
    rmdir(scratch);
    if (failures != 0) {
        printf(NAME ": FAIL failures=%d\n", failures);
        return 1;
    }
    printf(NAME ": PASS\n");
    return 0;
}
