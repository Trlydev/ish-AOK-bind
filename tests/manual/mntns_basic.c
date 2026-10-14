// Mount namespaces: a process with its own mount table.
//
// AOK had one mount table for the whole machine: clone(CLONE_NEWNS) was
// EINVAL and unshare(CLONE_NEWNS) ENOSYS. A sandbox is built out of exactly
// this -- bubblewrap unshares, mounts a tmpfs, binds what the program may see
// into it and pivots onto it -- and nothing else gives a process a private
// view of the filesystem.
//
// Each case runs in children of this process; the parent stays in the mount
// namespace it started in and is the witness for "nothing leaked out".
//
//   unshare      unshare(CLONE_NEWNS) succeeds, /proc/self/ns/mnt names a
//                different namespace from the parent's, and making the tree
//                private (MS_REC|MS_PRIVATE on /) is accepted
//   isolation    a tmpfs the child mounts, and a file in it, are not seen by
//                the parent; the child's /proc/self/mountinfo lists the
//                mount and the parent's does not
//   inherited    a mount made BEFORE the unshare is in the child too; the
//                child unmounting its copy leaves the parent's in place
//   later        a mount the parent makes after the child's unshare does not
//                appear in the child
//   clone        clone(CLONE_NEWNS) gives the same as fork + unshare
//   setns        a second process joins the child's namespace by
//                /proc/<pid>/ns/mnt and sees what the child mounted
//   read-only    a bind mount remounted MS_RDONLY refuses writes with EROFS
//                while the directory it is a bind of still takes them
//   pivot_root   onto a tmpfs: its files are at /, the old root is under the
//                directory named for it, and is gone after a lazy unmount
//   move         MS_MOVE takes what is mounted in a mount along with it, and
//                a mount cannot be moved into its own tree (ELOOP)
//   covered      mountinfo lists a mount that another covers; a namespace
//                copied while it is covered has it, under the copy of the
//                cover, and in view once that is unmounted there
//
// Needs root. On Linux, run by an ordinary user, it first enters a user
// namespace (where it is root over its own mounts): that is how it is run
// against a real kernel as the oracle. AOK has no user namespaces and runs
// it as root.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static char base[64];

#define CHECK(cond, ...) do { if (!(cond)) { printf("mntns_basic: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

// A child reports with its exit status: 0, or the line number of the first
// thing that went wrong (kept under 256 by the modulus; the message is
// printed by the child itself).
#define CHILD_CHECK(cond, ...) do { if (!(cond)) { printf("mntns_basic: " __VA_ARGS__); printf(" (errno %d: %s)\n", errno, strerror(errno)); fflush(stdout); _exit(1 + __LINE__ % 200); } } while (0)

static void path_in(char *out, size_t size, const char *name) {
    snprintf(out, size, "%s/%s", base, name);
}

static int exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static int touch(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

// Does /proc/self/mountinfo have a mount at `point`?
static int mountinfo_has(const char *point) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (f == NULL)
        return -1;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        // id parent major:minor root POINT options ...
        char field[5][512];
        if (sscanf(line, "%511s %511s %511s %511s %511s", field[0], field[1], field[2], field[3], field[4]) == 5 &&
                strcmp(field[4], point) == 0)
            found = 1;
    }
    fclose(f);
    return found;
}

static int ns_name(pid_t pid, char *out, size_t size) {
    char link[64];
    if (pid == 0)
        snprintf(link, sizeof(link), "/proc/self/ns/mnt");
    else
        snprintf(link, sizeof(link), "/proc/%d/ns/mnt", (int) pid);
    ssize_t n = readlink(link, out, size - 1);
    if (n < 0)
        return -1;
    out[n] = '\0';
    return 0;
}

static int wait_ok(pid_t pid, const char *what) {
    int status = -1;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("mntns_basic: %s: child status %#x\n", what, status);
        failures++;
        return -1;
    }
    return 0;
}

// Two pipes for a parent and child to take turns on. Each side closes the
// ends that are the other's, so that one side dying is an end-of-file to the
// one waiting for it and not a wait for ever.
struct turn { int to_child[2], to_parent[2]; };
static void turn_init(struct turn *t) { if (pipe(t->to_child) != 0 || pipe(t->to_parent) != 0) { perror("pipe"); exit(2); } }
static void turn_child(struct turn *t) { close(t->to_child[1]); close(t->to_parent[0]); }
static void turn_parent(struct turn *t) { close(t->to_child[0]); close(t->to_parent[1]); }
static void turn_wait(int fd) { char c; if (read(fd, &c, 1) != 1) _exit(250); }
static void turn_go(int fd) { if (write(fd, "x", 1) != 1) _exit(251); }
// The parent's wait for its child's turn: 0, or -1 if the child is gone
// (it has said why, and its exit status will be counted).
static int turn_wait_child(struct turn *t) { char c; return read(t->to_parent[0], &c, 1) == 1 ? 0 : -1; }
static void turn_done(struct turn *t) { (void) !write(t->to_child[1], "x", 1); close(t->to_child[1]); close(t->to_parent[0]); }

static void private_tree(void) {
    // What unshare(1) does: without it a mount made here can propagate back
    // to a parent whose tree is shared (a systemd host's is).
    CHILD_CHECK(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0, "making the tree private failed");
}

static void case_isolation(void) {
    char dir[128], inside[160], parent_ns[64];
    path_in(dir, sizeof(dir), "iso");
    snprintf(inside, sizeof(inside), "%s/inside", dir);
    mkdir(dir, 0755);
    CHECK(ns_name(0, parent_ns, sizeof(parent_ns)) == 0, "cannot read /proc/self/ns/mnt: %s", strerror(errno));
    struct turn t;
    turn_init(&t);
    pid_t pid = fork();
    if (pid == 0) {
        char child_ns[64];
        turn_child(&t);
        CHILD_CHECK(unshare(CLONE_NEWNS) == 0, "unshare(CLONE_NEWNS) failed");
        CHILD_CHECK(ns_name(0, child_ns, sizeof(child_ns)) == 0, "no /proc/self/ns/mnt after unshare");
        CHILD_CHECK(strcmp(child_ns, parent_ns) != 0, "after unshare the namespace is still %s", child_ns);
        private_tree();
        CHILD_CHECK(mount("mntns", dir, "tmpfs", 0, NULL) == 0, "mount tmpfs on %s failed", dir);
        CHILD_CHECK(touch(inside) == 0, "cannot create a file in the new tmpfs");
        CHILD_CHECK(mountinfo_has(dir) == 1, "the child's mountinfo does not list %s", dir);
        turn_go(t.to_parent[1]);
        turn_wait(t.to_child[0]);
        CHILD_CHECK(exists(inside), "the child's own mount lost its file");
        _exit(0);
    }
    turn_parent(&t);
    if (turn_wait_child(&t) == 0) {
        CHECK(!exists(inside), "isolation: the parent sees the file the child put in its private tmpfs");
        CHECK(mountinfo_has(dir) == 0, "isolation: the parent's mountinfo lists the child's mount at %s", dir);
        char child_ns[64] = "";
        CHECK(ns_name(pid, child_ns, sizeof(child_ns)) == 0 && strcmp(child_ns, parent_ns) != 0,
              "isolation: /proc/<child>/ns/mnt reads \"%s\", the parent's is \"%s\"", child_ns, parent_ns);
    }
    turn_done(&t);
    wait_ok(pid, "isolation");
}

static void case_inherited_and_later(void) {
    char early[128], early_file[160], late[128], late_file[160];
    path_in(early, sizeof(early), "early");
    path_in(late, sizeof(late), "late");
    snprintf(early_file, sizeof(early_file), "%s/here", early);
    snprintf(late_file, sizeof(late_file), "%s/here", late);
    mkdir(early, 0755);
    mkdir(late, 0755);
    CHECK(mount("mntns", early, "tmpfs", 0, NULL) == 0, "inherited: the parent could not mount a tmpfs: %s", strerror(errno));
    CHECK(touch(early_file) == 0, "inherited: cannot create a file in the parent's tmpfs");
    struct turn t;
    turn_init(&t);
    pid_t pid = fork();
    if (pid == 0) {
        turn_child(&t);
        CHILD_CHECK(unshare(CLONE_NEWNS) == 0, "unshare(CLONE_NEWNS) failed");
        private_tree();
        CHILD_CHECK(exists(early_file), "a mount made before the unshare is not in the new namespace");
        CHILD_CHECK(umount(early) == 0, "the child could not unmount its copy of %s", early);
        CHILD_CHECK(!exists(early_file), "the file is still there after the child's umount");
        turn_go(t.to_parent[1]);
        turn_wait(t.to_child[0]);
        CHILD_CHECK(!exists(late_file), "a mount the parent made after the unshare appeared in the child");
        _exit(0);
    }
    turn_parent(&t);
    if (turn_wait_child(&t) == 0) {
        CHECK(exists(early_file), "inherited: the child's umount took the parent's mount away");
        CHECK(mount("mntns", late, "tmpfs", 0, NULL) == 0, "later: the parent could not mount: %s", strerror(errno));
        CHECK(touch(late_file) == 0, "later: cannot create a file");
    }
    turn_done(&t);
    wait_ok(pid, "inherited/later");
    umount(late);
    umount(early);
}

static int clone_child(void *arg) {
    const char *dir = arg;
    char inside[160];
    snprintf(inside, sizeof(inside), "%s/inside", dir);
    private_tree();
    CHILD_CHECK(mount("mntns", dir, "tmpfs", 0, NULL) == 0, "mount in a clone(CLONE_NEWNS) child failed");
    CHILD_CHECK(touch(inside) == 0, "cannot create a file");
    return 0;
}

static void case_clone(void) {
    static char dir[128];
    char inside[160];
    path_in(dir, sizeof(dir), "cloned");
    snprintf(inside, sizeof(inside), "%s/inside", dir);
    mkdir(dir, 0755);
    static char stack[256 * 1024];
    pid_t pid = clone(clone_child, stack + sizeof(stack), CLONE_NEWNS | SIGCHLD, dir);
    CHECK(pid > 0, "clone(CLONE_NEWNS|SIGCHLD) failed: %s", strerror(errno));
    if (pid <= 0)
        return;
    wait_ok(pid, "clone");
    CHECK(!exists(inside), "clone: the parent sees the file from the child's namespace");
}

static void case_setns(void) {
    char dir[128], inside[160];
    path_in(dir, sizeof(dir), "joined");
    snprintf(inside, sizeof(inside), "%s/inside", dir);
    mkdir(dir, 0755);
    struct turn t;
    turn_init(&t);
    pid_t holder = fork();
    if (holder == 0) {
        turn_child(&t);
        CHILD_CHECK(unshare(CLONE_NEWNS) == 0, "unshare(CLONE_NEWNS) failed");
        private_tree();
        CHILD_CHECK(mount("mntns", dir, "tmpfs", 0, NULL) == 0, "mount failed");
        CHILD_CHECK(touch(inside) == 0, "cannot create a file");
        turn_go(t.to_parent[1]);
        turn_wait(t.to_child[0]);
        _exit(0);
    }
    turn_parent(&t);
    if (turn_wait_child(&t) != 0) {
        turn_done(&t);
        wait_ok(holder, "setns holder");
        return;
    }
    pid_t joiner = fork();
    if (joiner == 0) {
        char link[64];
        snprintf(link, sizeof(link), "/proc/%d/ns/mnt", (int) holder);
        int fd = open(link, O_RDONLY);
        CHILD_CHECK(fd >= 0, "cannot open %s", link);
        CHILD_CHECK(!exists(inside), "the joiner sees the file before joining");
        CHILD_CHECK(setns(fd, CLONE_NEWNS) == 0, "setns to the holder's mount namespace failed");
        CHILD_CHECK(exists(inside), "after setns the holder's mount is not visible");
        _exit(0);
    }
    wait_ok(joiner, "setns");
    turn_done(&t);
    wait_ok(holder, "setns holder");
}

static void case_readonly_bind(void) {
    char src[128], dst[128], via_src[160], via_dst[160];
    path_in(src, sizeof(src), "rosrc");
    path_in(dst, sizeof(dst), "rodst");
    snprintf(via_src, sizeof(via_src), "%s/f", src);
    snprintf(via_dst, sizeof(via_dst), "%s/g", dst);
    mkdir(src, 0755);
    mkdir(dst, 0755);
    pid_t pid = fork();
    if (pid == 0) {
        CHILD_CHECK(unshare(CLONE_NEWNS) == 0, "unshare(CLONE_NEWNS) failed");
        private_tree();
        // Its own filesystem underneath, so the bind is of a mount this
        // namespace owns (a user namespace may not remount what it was given).
        CHILD_CHECK(mount("mntns", src, "tmpfs", 0, NULL) == 0, "mount tmpfs failed");
        CHILD_CHECK(mount(src, dst, NULL, MS_BIND, NULL) == 0, "bind mount failed");
        CHILD_CHECK(touch(via_dst) == 0, "a fresh bind mount is not writable");
        CHILD_CHECK(mount(NULL, dst, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) == 0, "remounting the bind read-only failed");
        errno = 0;
        int fd = open(via_dst, O_WRONLY);
        CHILD_CHECK(fd < 0 && errno == EROFS, "opening a file for writing through the read-only bind gave fd %d", fd);
        errno = 0;
        CHILD_CHECK(mkdir(dst, 0755) != 0, "mkdir of an existing directory succeeded");
        char sub[200];
        snprintf(sub, sizeof(sub), "%s/newdir", dst);
        errno = 0;
        CHILD_CHECK(mkdir(sub, 0755) != 0 && errno == EROFS, "mkdir through the read-only bind did not fail with EROFS");
        errno = 0;
        CHILD_CHECK(unlink(via_dst) != 0 && errno == EROFS, "unlink through the read-only bind did not fail with EROFS");
        CHILD_CHECK(touch(via_src) == 0, "the directory the bind is of stopped taking writes");
        snprintf(sub, sizeof(sub), "%s/f", dst);
        CHILD_CHECK(exists(sub), "a file written through the source is not seen through the bind");
        _exit(0);
    }
    wait_ok(pid, "read-only bind");
}

static void case_move(void) {
    char from[128], to[128], sub[160], inner[200], moved_inner[200], left[200], into[200];
    path_in(from, sizeof(from), "move-from");
    path_in(to, sizeof(to), "move-to");
    snprintf(sub, sizeof(sub), "%s/sub", from);
    snprintf(inner, sizeof(inner), "%s/sub/inner", from);
    snprintf(moved_inner, sizeof(moved_inner), "%s/sub/inner", to);
    snprintf(left, sizeof(left), "%s/sub", from);
    snprintf(into, sizeof(into), "%s/sub", to);
    mkdir(from, 0755);
    mkdir(to, 0755);
    pid_t pid = fork();
    if (pid == 0) {
        CHILD_CHECK(unshare(CLONE_NEWNS) == 0, "unshare(CLONE_NEWNS) failed");
        private_tree();
        CHILD_CHECK(mount("outer", from, "tmpfs", 0, NULL) == 0, "mount tmpfs on %s failed", from);
        CHILD_CHECK(mkdir(sub, 0755) == 0, "mkdir %s failed", sub);
        CHILD_CHECK(mount("inner", sub, "tmpfs", 0, NULL) == 0, "mount tmpfs on %s failed", sub);
        CHILD_CHECK(touch(inner) == 0, "cannot create a file in the inner tmpfs");
        CHILD_CHECK(mount(from, to, NULL, MS_MOVE, NULL) == 0, "MS_MOVE of %s to %s failed", from, to);
        CHILD_CHECK(exists(moved_inner), "the mount inside the moved one did not come with it: no %s", moved_inner);
        CHILD_CHECK(!exists(left), "something is still at %s after the move", left);
        CHILD_CHECK(mountinfo_has(into) == 1 && mountinfo_has(to) == 1, "mountinfo does not list the moved mounts at %s and %s", to, into);
        CHILD_CHECK(mountinfo_has(sub) == 0 && mountinfo_has(from) == 0, "mountinfo still lists a mount at %s or %s", from, sub);
        errno = 0;
        CHILD_CHECK(mount(to, into, NULL, MS_MOVE, NULL) == -1 && errno == ELOOP, "moving a mount into its own tree did not fail with ELOOP");
        // The inner one is still a mount of its own: it goes, and its file
        // with it, leaving the directory it was on.
        CHILD_CHECK(umount(into) == 0, "unmounting the inner mount at its new place failed");
        CHILD_CHECK(!exists(moved_inner) && exists(into), "after unmounting the inner mount, %s is %s", into, exists(into) ? "there with its file" : "gone");
        _exit(0);
    }
    wait_ok(pid, "move");
}

static int count_at(const char *point) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (f == NULL)
        return -1;
    char line[1024], field[5][512];
    int n = 0;
    while (fgets(line, sizeof(line), f) != NULL)
        if (sscanf(line, "%511s %511s %511s %511s %511s", field[0], field[1], field[2], field[3], field[4]) == 5 &&
                strcmp(field[4], point) == 0)
            n++;
    fclose(f);
    return n;
}

static void case_covered(void) {
    char d[128], d_s[160], marker[200];
    path_in(d, sizeof(d), "covered");
    snprintf(d_s, sizeof(d_s), "%s/s", d);
    snprintf(marker, sizeof(marker), "%s/s/marker", d);
    mkdir(d, 0755);
    pid_t pid = fork();
    if (pid == 0) {
        CHILD_CHECK(unshare(CLONE_NEWNS) == 0, "unshare(CLONE_NEWNS) failed");
        private_tree();
        CHILD_CHECK(mount("outer", d, "tmpfs", 0, NULL) == 0, "mount failed");
        CHILD_CHECK(mkdir(d_s, 0755) == 0 && mount("inner", d_s, "tmpfs", 0, NULL) == 0 && touch(marker) == 0, "the inner mount failed");
        CHILD_CHECK(mount("lid", d, "tmpfs", 0, NULL) == 0, "the lid failed");
        CHILD_CHECK(!exists(marker), "the covered mount shows through the lid");
        CHILD_CHECK(count_at(d) == 2 && count_at(d_s) == 1, "mountinfo lists %d mounts at %s and %d at %s; wanted 2 and the covered 1",
                    count_at(d), d, count_at(d_s), d_s);
        pid_t inner = fork();
        if (inner == 0) {
            CHILD_CHECK(unshare(CLONE_NEWNS) == 0, "the second unshare failed");
            CHILD_CHECK(count_at(d_s) == 1, "the copy has %d mounts at the covered %s", count_at(d_s), d_s);
            CHILD_CHECK(umount(d) == 0, "unmounting the lid in the copy failed");
            CHILD_CHECK(exists(marker), "in the copy, the covered mount is not there once the lid is gone");
            _exit(0);
        }
        int status = -1;
        CHILD_CHECK(waitpid(inner, &status, 0) == inner && WIFEXITED(status) && WEXITSTATUS(status) == 0, "the copy's check failed: %#x", status);
        CHILD_CHECK(!exists(marker) && count_at(d) == 2, "the copy's unmount reached the namespace it was copied from");
        _exit(0);
    }
    wait_ok(pid, "covered");
}

static void case_pivot_root(void) {
    char newroot[128];
    path_in(newroot, sizeof(newroot), "newroot");
    mkdir(newroot, 0755);
    pid_t pid = fork();
    if (pid == 0) {
        char path[200];
        CHILD_CHECK(unshare(CLONE_NEWNS) == 0, "unshare(CLONE_NEWNS) failed");
        private_tree();
        CHILD_CHECK(mount("mntns", newroot, "tmpfs", 0, NULL) == 0, "mount tmpfs for the new root failed");
        snprintf(path, sizeof(path), "%s/marker", newroot);
        CHILD_CHECK(touch(path) == 0, "cannot create a file in the new root");
        snprintf(path, sizeof(path), "%s/old", newroot);
        CHILD_CHECK(mkdir(path, 0755) == 0, "cannot make the directory for the old root");
        CHILD_CHECK(syscall(SYS_pivot_root, newroot, path) == 0, "pivot_root failed");
        CHILD_CHECK(chdir("/") == 0, "chdir to the new root failed");
        CHILD_CHECK(exists("/marker"), "after pivot_root the new root's file is not at /marker");
        CHILD_CHECK(exists("/old/proc") || exists("/old/etc") || exists("/old/tmp"), "the old root is not under /old");
        CHILD_CHECK(!exists("/etc/passwd") || exists("/marker"), "impossible");
        CHILD_CHECK(!exists("/etc"), "the old root's /etc is still at /etc");
        CHILD_CHECK(umount2("/old", MNT_DETACH) == 0, "lazy unmount of the old root failed");
        CHILD_CHECK(!exists("/old/proc") && !exists("/old/etc") && !exists("/old/tmp"), "the old root is still under /old after its unmount");
        CHILD_CHECK(exists("/marker"), "the new root lost its file");
        _exit(0);
    }
    wait_ok(pid, "pivot_root");
    char path[200];
    snprintf(path, sizeof(path), "%s/marker", newroot);
    CHECK(!exists(path), "pivot_root: the parent sees the child's new root");
    CHECK(exists("/etc"), "pivot_root: the parent's own root changed");
}

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t) strlen(text) ? 0 : -1;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN); // a child that is gone is a failed case, not a dead test
    if (geteuid() != 0) {
#ifdef __linux__
        // The oracle run: root over a user namespace's own mounts.
        uid_t uid = getuid();
        gid_t gid = getgid();
        char map[64];
        if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
            printf("mntns_basic: SKIP (not root, and no user namespace to be root in: %s)\n", strerror(errno));
            return 0;
        }
        snprintf(map, sizeof(map), "0 %d 1", (int) uid);
        write_file("/proc/self/setgroups", "deny");
        snprintf(map, sizeof(map), "0 %d 1", (int) uid);
        if (write_file("/proc/self/uid_map", map) != 0) {
            printf("mntns_basic: SKIP (cannot write uid_map: %s)\n", strerror(errno));
            return 0;
        }
        snprintf(map, sizeof(map), "0 %d 1", (int) gid);
        write_file("/proc/self/gid_map", map);
        if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
            printf("mntns_basic: SKIP (cannot make the tree private: %s)\n", strerror(errno));
            return 0;
        }
#else
        printf("mntns_basic: SKIP (needs root)\n");
        return 0;
#endif
    }
    // Everything happens under a tmpfs of this test's own, so the mounts it
    // makes are on a filesystem it owns and nothing is left behind.
    // (A name of its own each run: a guest's pids start over with every boot.)
    snprintf(base, sizeof(base), "/tmp/mntns_basic.XXXXXX");
    if (mkdtemp(base) == NULL || chmod(base, 0755) != 0 || mount("mntns", base, "tmpfs", 0, NULL) != 0) {
        printf("mntns_basic: FAIL setup: cannot make and mount %s: %s\n", base, strerror(errno));
        rmdir(base);
        return 1;
    }

    case_isolation();
    case_inherited_and_later();
    case_clone();
    case_setns();
    case_readonly_bind();
    case_pivot_root();
    case_move();
    case_covered();

    umount2(base, MNT_DETACH);
    rmdir(base);
    if (failures != 0) {
        printf("mntns_basic: FAIL failures=%d\n", failures);
        return 1;
    }
    printf("mntns_basic: PASS\n");
    return 0;
}
