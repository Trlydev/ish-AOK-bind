// Mount propagation: shared, slave, private and unbindable mounts.
//
// AOK's mounts were all private. mount --make-shared was accepted and did
// nothing, a bind of a mount was no peer of it, and a mount made in one
// namespace never appeared in another -- so a host mount made after a
// service started was not in the service's view, `findmnt -o PROPAGATION`
// said "private" of everything, and a container runtime that asks for
// rshared or rslave binds got neither. fs/mount.c has the model: a peer
// group is a number mounts share, a slave names the group it receives from,
// and a mount or unmount under a shared mount is repeated at the same place
// in every mount that receives from it.
//
// Every expectation below is Linux 6.12's, where the test runs as an
// ordinary user inside a user and mount namespace of its own (camd). On AOK
// it runs as root, in a mount namespace of its own made private first.
//
//   shared       make-shared tags the mount "shared:N" in mountinfo; a bind
//                of it is in the same group; a mount under one appears under
//                the other, in a group of its own that its copy shares; an
//                unmount under one takes the other's copy too
//   slave        make-slave leaves the group and names it "master:N"; a mount
//                under the master appears under the slave (as a slave of the
//                new mount's group), one under the slave appears nowhere
//                else; an unmount under the master reaches the slave
//   both         a slave made shared again is "shared:M master:N"
//   private      make-private: nothing arrives and nothing leaves
//   recursive    MS_REC changes every mount below as well
//   unbindable   cannot be bound (EINVAL)
//   refused      make-shared of what is not a mount point is EINVAL; a mount
//                under a shared mount cannot be moved (EINVAL), and
//                pivot_root is EINVAL while the new root's parent mount is
//                shared, or the new root itself is, and works once neither is
//   part         a bind of a directory inside a shared mount is a peer that
//                shows part of it: what is mounted in that part appears in
//                both, what is mounted elsewhere in the mount does not
//   trees        a recursive bind into a shared mount arrives under its
//                peers whole, submounts and all; so does a mount moved in;
//                a lazy unmount takes the copies' whole trees
//   chain        the peers of a slave that is shared again all receive, and
//                their copies are a group of their own, slaves of the
//                mount's
//   setattr      mount_setattr changes propagation as mount(2) does
//   namespaces   a mount namespace copied from a tree with a shared mount has
//                a peer of it: a mount made in either appears in the other.
//                Made slave there, it only receives. Copied into a new USER
//                namespace it is a slave from the start; what arrives in it
//                can be unmounted there (it covers nothing that was hidden),
//                and that unmount goes nowhere else
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define NAME "mount_propagation"
static int failures;
static char base[64];

#define CHECK(cond, ...) do { if (!(cond)) { printf(NAME ": " __VA_ARGS__); printf(" (errno %d: %s)\n", errno, strerror(errno)); failures++; } } while (0)
#define CHILD_CHECK(cond, ...) do { if (!(cond)) { printf(NAME ": " __VA_ARGS__); printf(" (errno %d: %s)\n", errno, strerror(errno)); fflush(stdout); _exit(1 + __LINE__ % 200); } } while (0)

static const char *in(const char *name) {
    static char paths[8][160];
    static int next;
    char *out = paths[next++ % 8];
    snprintf(out, sizeof(paths[0]), "%s/%s", base, name);
    return out;
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

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return errno;
    ssize_t n = write(fd, text, strlen(text));
    int err = n < 0 ? errno : 0;
    close(fd);
    return err;
}

// The mount at `point` in /proc/self/mountinfo (the last one listed there):
// its peer group and its master's, 0 for none; -1 when there is no mount.
struct tags { int shared, master, unbindable; };
static int tags_of(const char *point, struct tags *out) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (f == NULL)
        return -1;
    char line[2048];
    int found = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        // id parent major:minor root POINT options [optional fields] - type source super
        char field[6][512];
        int used = 0;
        if (sscanf(line, "%511s %511s %511s %511s %511s %511s%n", field[0], field[1], field[2], field[3], field[4], field[5], &used) != 6 ||
                strcmp(field[4], point) != 0)
            continue;
        *out = (struct tags) {0, 0, 0};
        char *rest = line + used, *end = strstr(rest, " - ");
        if (end != NULL)
            *end = '\0';
        char *at;
        if ((at = strstr(rest, "shared:")) != NULL)
            out->shared = atoi(at + 7);
        if ((at = strstr(rest, "master:")) != NULL)
            out->master = atoi(at + 7);
        out->unbindable = strstr(rest, "unbindable") != NULL;
        found = 0;
    }
    fclose(f);
    return found;
}

static int shared_of(const char *point) {
    struct tags t;
    return tags_of(point, &t) == 0 ? t.shared : -1;
}
static int master_of(const char *point) {
    struct tags t;
    return tags_of(point, &t) == 0 ? t.master : -1;
}
static int mounted(const char *point) {
    struct tags t;
    return tags_of(point, &t) == 0;
}

static int tmpfs_at(const char *point) {
    mkdir(point, 0755);
    return mount("prop", point, "tmpfs", 0, NULL);
}

static int wait_ok(pid_t pid, const char *what) {
    int status = -1;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf(NAME ": %s: child status %#x\n", what, status);
        failures++;
        return -1;
    }
    return 0;
}

// A parent and child taking turns: see tests/manual/mntns_basic.c.
struct turn { int to_child[2], to_parent[2]; };
static void turn_init(struct turn *t) { if (pipe(t->to_child) != 0 || pipe(t->to_parent) != 0) { perror("pipe"); exit(2); } }
static void turn_child(struct turn *t) { close(t->to_child[1]); close(t->to_parent[0]); }
static void turn_parent(struct turn *t) { close(t->to_child[0]); close(t->to_parent[1]); }
static void child_waits(struct turn *t) { char c; if (read(t->to_child[0], &c, 1) != 1) _exit(250); }
static void child_says(struct turn *t) { if (write(t->to_parent[1], "x", 1) != 1) _exit(251); }
static int parent_waits(struct turn *t) { char c; return read(t->to_parent[0], &c, 1) == 1 ? 0 : -1; }
static void parent_says(struct turn *t) { (void) !write(t->to_child[1], "x", 1); }
static void turn_done(struct turn *t) { close(t->to_child[1]); close(t->to_parent[0]); }

static void case_shared(void) {
    const char *a = in("sh-a"), *b = in("sh-b");
    CHECK(tmpfs_at(a) == 0, "mount %s failed", a);
    mkdir(b, 0755);
    CHECK(shared_of(a) == 0 && master_of(a) == 0, "a new mount is not private: shared:%d master:%d", shared_of(a), master_of(a));
    CHECK(mount(NULL, a, NULL, MS_SHARED, NULL) == 0, "make-shared failed");
    int group = shared_of(a);
    CHECK(group > 0, "after make-shared mountinfo says shared:%d", group);
    CHECK(mount(a, b, NULL, MS_BIND, NULL) == 0, "bind of the shared mount failed");
    CHECK(shared_of(b) == group, "a bind of a shared mount is shared:%d, its source shared:%d", shared_of(b), group);

    // A mount under one appears under the other.
    char a_sub[200], b_sub[200], a_file[220], b_file[220];
    snprintf(a_sub, sizeof(a_sub), "%s/sub", a);
    snprintf(b_sub, sizeof(b_sub), "%s/sub", b);
    snprintf(a_file, sizeof(a_file), "%s/sub/file", a);
    snprintf(b_file, sizeof(b_file), "%s/sub/file", b);
    mkdir(a_sub, 0755);
    CHECK(exists(b_sub), "the two do not show the same directory");
    CHECK(mount("inner", a_sub, "tmpfs", 0, NULL) == 0, "mount under the shared mount failed");
    CHECK(mounted(b_sub), "a mount made under one peer did not appear under the other");
    CHECK(touch(a_file) == 0 && exists(b_file), "the mount that appeared under the peer is not the same filesystem");
    int inner = shared_of(a_sub);
    CHECK(inner > 0 && inner != group && shared_of(b_sub) == inner,
          "the new mount is shared:%d and its copy shared:%d (the mounts above them are shared:%d)", inner, shared_of(b_sub), group);

    // ...and an unmount under one takes the other's.
    CHECK(umount(b_sub) == 0, "unmounting the copy failed");
    CHECK(!mounted(a_sub) && !mounted(b_sub), "after unmounting under one peer, %s is %s and %s is %s",
          a_sub, mounted(a_sub) ? "mounted" : "gone", b_sub, mounted(b_sub) ? "mounted" : "gone");
    umount(b);
    umount(a);
}

static void case_slave(void) {
    const char *a = in("sl-a"), *b = in("sl-b");
    char a_x[200], b_x[200], a_y[200], b_y[200];
    snprintf(a_x, sizeof(a_x), "%s/x", a);
    snprintf(b_x, sizeof(b_x), "%s/x", b);
    snprintf(a_y, sizeof(a_y), "%s/y", a);
    snprintf(b_y, sizeof(b_y), "%s/y", b);
    CHECK(tmpfs_at(a) == 0 && mount(NULL, a, NULL, MS_SHARED, NULL) == 0, "a shared mount at %s failed", a);
    mkdir(b, 0755);
    mkdir(a_x, 0755);
    mkdir(a_y, 0755);
    CHECK(mount(a, b, NULL, MS_BIND, NULL) == 0, "bind failed");
    int group = shared_of(a);
    CHECK(mount(NULL, b, NULL, MS_SLAVE, NULL) == 0, "make-slave failed");
    CHECK(shared_of(b) == 0 && master_of(b) == group, "after make-slave the bind is shared:%d master:%d, wanted master:%d", shared_of(b), master_of(b), group);
    CHECK(shared_of(a) == group, "the master lost its group: shared:%d", shared_of(a));

    CHECK(mount("down", a_x, "tmpfs", 0, NULL) == 0, "mount under the master failed");
    CHECK(mounted(b_x), "a mount under the master did not appear under the slave");
    int down = shared_of(a_x);
    CHECK(down > 0 && shared_of(b_x) == 0 && master_of(b_x) == down,
          "under the master the mount is shared:%d; its copy under the slave is shared:%d master:%d", down, shared_of(b_x), master_of(b_x));

    CHECK(mount("up", b_y, "tmpfs", 0, NULL) == 0, "mount under the slave failed");
    CHECK(!mounted(a_y), "a mount under the slave appeared under its master");
    CHECK(shared_of(b_y) == 0 && master_of(b_y) == 0, "a mount under a slave is shared:%d master:%d, wanted private", shared_of(b_y), master_of(b_y));

    CHECK(umount(a_x) == 0, "unmount under the master failed");
    CHECK(!mounted(b_x), "an unmount under the master left the slave's copy");

    // A slave made shared again is both.
    CHECK(mount(NULL, b, NULL, MS_SHARED, NULL) == 0, "make-shared of a slave failed");
    CHECK(shared_of(b) > 0 && shared_of(b) != group && master_of(b) == group,
          "a slave made shared is shared:%d master:%d, wanted a group of its own and master:%d", shared_of(b), master_of(b), group);

    // Private: nothing arrives.
    CHECK(mount(NULL, b, NULL, MS_PRIVATE, NULL) == 0, "make-private failed");
    CHECK(shared_of(b) == 0 && master_of(b) == 0, "after make-private: shared:%d master:%d", shared_of(b), master_of(b));
    CHECK(mount("late", a_x, "tmpfs", 0, NULL) == 0, "mount under the master failed");
    CHECK(!mounted(b_x), "a mount appeared under a private mount");
    umount(a_x);
    umount(b_y);
    umount(b);
    umount(a);
}

static void case_recursive_and_refused(void) {
    const char *a = in("rec-a"), *b = in("rec-b"), *plain = in("rec-dir"), *to = in("rec-to");
    char a_sub[200];
    snprintf(a_sub, sizeof(a_sub), "%s/sub", a);
    mkdir(b, 0755);
    mkdir(plain, 0755);
    mkdir(to, 0755);
    CHECK(tmpfs_at(a) == 0 && tmpfs_at(a_sub) == 0, "mounts failed");
    CHECK(mount(NULL, a, NULL, MS_SHARED | MS_REC, NULL) == 0, "make-rshared failed");
    CHECK(shared_of(a) > 0 && shared_of(a_sub) > 0 && shared_of(a) != shared_of(a_sub),
          "after make-rshared: shared:%d and, below it, shared:%d", shared_of(a), shared_of(a_sub));

    // What sits under a shared mount cannot be moved.
    errno = 0;
    CHECK(mount(a_sub, to, NULL, MS_MOVE, NULL) == -1 && errno == EINVAL, "moving a mount out from under a shared mount did not fail with EINVAL");

    CHECK(mount(NULL, a, NULL, MS_PRIVATE | MS_REC, NULL) == 0, "make-rprivate failed");
    CHECK(shared_of(a) == 0 && shared_of(a_sub) == 0, "after make-rprivate: shared:%d and shared:%d", shared_of(a), shared_of(a_sub));

    errno = 0;
    CHECK(mount(NULL, plain, NULL, MS_SHARED, NULL) == -1 && errno == EINVAL, "make-shared of a directory that is no mount point did not fail with EINVAL");

    struct tags t = {0, 0, 0};
    CHECK(mount(NULL, a_sub, NULL, MS_UNBINDABLE, NULL) == 0, "make-unbindable failed");
    CHECK(tags_of(a_sub, &t) == 0 && t.unbindable && t.shared == 0, "after make-unbindable mountinfo does not say so");
    errno = 0;
    CHECK(mount(a_sub, b, NULL, MS_BIND, NULL) == -1 && errno == EINVAL, "binding an unbindable mount did not fail with EINVAL");
    umount(a_sub);
    umount(a);
}

static void case_part(void) {
    const char *a = in("pt-a"), *b = in("pt-b");
    char a_d[200], a_d_x[220], b_x[200], a_other[200], b_y[200], a_d_y[220];
    snprintf(a_d, sizeof(a_d), "%s/d", a);
    snprintf(a_d_x, sizeof(a_d_x), "%s/d/x", a);
    snprintf(a_d_y, sizeof(a_d_y), "%s/d/y", a);
    snprintf(a_other, sizeof(a_other), "%s/other", a);
    snprintf(b_x, sizeof(b_x), "%s/x", b);
    snprintf(b_y, sizeof(b_y), "%s/y", b);
    CHECK(tmpfs_at(a) == 0 && mount(NULL, a, NULL, MS_SHARED, NULL) == 0, "a shared mount failed");
    mkdir(a_d, 0755);
    mkdir(a_d_x, 0755);
    mkdir(a_d_y, 0755);
    mkdir(a_other, 0755);
    mkdir(b, 0755);
    CHECK(mount(a_d, b, NULL, MS_BIND, NULL) == 0, "bind of a directory in the shared mount failed");
    CHECK(shared_of(b) == shared_of(a), "a bind of part of a shared mount is shared:%d, the mount shared:%d", shared_of(b), shared_of(a));
    CHECK(mount("in", a_d_x, "tmpfs", 0, NULL) == 0, "mount failed");
    CHECK(mounted(b_x), "a mount inside the part both show did not appear in the bind");
    CHECK(mount("out", a_other, "tmpfs", 0, NULL) == 0, "mount failed");
    CHECK(mount("back", b_y, "tmpfs", 0, NULL) == 0, "mount under the bind failed");
    CHECK(mounted(a_d_y), "a mount under the bind did not appear in the mount it is part of");
    CHECK(umount(a_d_y) == 0 && !mounted(b_y), "unmounting it there did not take the bind's");
    umount(a_other);
    umount(a_d_x);
    umount(b);
    umount(a);
}

static void case_trees(void) {
    const char *s = in("tr-s"), *s2 = in("tr-s2"), *t = in("tr-t"), *m = in("tr-m");
    char t_sub[200], s_in[200], s_in_sub[220], s2_in[200], s2_in_sub[220], s_mv[200], s2_mv[200], file[240];
    snprintf(t_sub, sizeof(t_sub), "%s/sub", t);
    snprintf(s_in, sizeof(s_in), "%s/in", s);
    snprintf(s_in_sub, sizeof(s_in_sub), "%s/in/sub", s);
    snprintf(s2_in, sizeof(s2_in), "%s/in", s2);
    snprintf(s2_in_sub, sizeof(s2_in_sub), "%s/in/sub", s2);
    snprintf(s_mv, sizeof(s_mv), "%s/mv", s);
    snprintf(s2_mv, sizeof(s2_mv), "%s/mv", s2);
    CHECK(tmpfs_at(s) == 0 && mount(NULL, s, NULL, MS_SHARED, NULL) == 0, "a shared mount failed");
    mkdir(s_in, 0755);
    mkdir(s_mv, 0755);
    mkdir(s2, 0755);
    CHECK(mount(s, s2, NULL, MS_BIND, NULL) == 0, "bind failed");
    CHECK(tmpfs_at(t) == 0 && tmpfs_at(t_sub) == 0, "the tree to bind failed");
    snprintf(file, sizeof(file), "%s/deep", t_sub);
    touch(file);

    CHECK(mount(t, s_in, NULL, MS_BIND | MS_REC, NULL) == 0, "recursive bind into the shared mount failed");
    CHECK(mounted(s_in) && mounted(s_in_sub), "the recursive bind did not bring its submount");
    CHECK(mounted(s2_in) && mounted(s2_in_sub), "under the peer: %s is %s, %s is %s", s2_in, mounted(s2_in) ? "mounted" : "missing",
          s2_in_sub, mounted(s2_in_sub) ? "mounted" : "missing");
    snprintf(file, sizeof(file), "%s/deep", s2_in_sub);
    CHECK(exists(file), "the submount that arrived under the peer is not the filesystem that was bound");
    CHECK(shared_of(s_in) > 0 && shared_of(s2_in) == shared_of(s_in) && shared_of(s_in_sub) > 0 && shared_of(s2_in_sub) == shared_of(s_in_sub),
          "groups: bind %d / copy %d, its submount %d / copy %d", shared_of(s_in), shared_of(s2_in), shared_of(s_in_sub), shared_of(s2_in_sub));

    // A lazy unmount takes the whole tree, and the copy's.
    CHECK(umount2(s_in, MNT_DETACH) == 0, "lazy unmount failed");
    CHECK(!mounted(s_in) && !mounted(s_in_sub) && !mounted(s2_in) && !mounted(s2_in_sub), "after a lazy unmount something of the tree or its copy is still mounted");

    // A mount moved in.
    CHECK(tmpfs_at(m) == 0, "mount failed");
    CHECK(mount(m, s_mv, NULL, MS_MOVE, NULL) == 0, "moving a mount into the shared mount failed");
    CHECK(mounted(s_mv) && mounted(s2_mv), "a mount moved in is %s there and %s under the peer", mounted(s_mv) ? "mounted" : "missing", mounted(s2_mv) ? "mounted" : "missing");
    CHECK(umount(s2_mv) == 0 && !mounted(s_mv), "unmounting the copy did not take the mount that was moved in");
    umount(t_sub);
    umount(t);
    umount(s2);
    umount(s);
}

static void case_chain(void) {
    const char *a = in("ch-a"), *b = in("ch-b"), *c = in("ch-c");
    char a_z[200], b_z[200], c_z[200];
    snprintf(a_z, sizeof(a_z), "%s/z", a);
    snprintf(b_z, sizeof(b_z), "%s/z", b);
    snprintf(c_z, sizeof(c_z), "%s/z", c);
    CHECK(tmpfs_at(a) == 0 && mount(NULL, a, NULL, MS_SHARED, NULL) == 0, "a shared mount failed");
    mkdir(a_z, 0755);
    mkdir(b, 0755);
    mkdir(c, 0755);
    int top = shared_of(a);
    CHECK(mount(a, b, NULL, MS_BIND, NULL) == 0 && mount(NULL, b, NULL, MS_SLAVE, NULL) == 0 &&
          mount(NULL, b, NULL, MS_SHARED, NULL) == 0, "a slave made shared failed");
    CHECK(mount(b, c, NULL, MS_BIND, NULL) == 0, "bind of the shared slave failed");
    int lower = shared_of(b);
    CHECK(lower > 0 && lower != top && shared_of(c) == lower && master_of(c) == top && master_of(b) == top,
          "b is shared:%d master:%d, its bind c shared:%d master:%d (a is shared:%d)", lower, master_of(b), shared_of(c), master_of(c), top);
    CHECK(mount("z", a_z, "tmpfs", 0, NULL) == 0, "mount under the top failed");
    CHECK(mounted(b_z) && mounted(c_z), "under the shared slaves: %s, %s", mounted(b_z) ? "mounted" : "missing", mounted(c_z) ? "mounted" : "missing");
    int z = shared_of(a_z);
    CHECK(z > 0 && shared_of(b_z) > 0 && shared_of(b_z) != z && shared_of(c_z) == shared_of(b_z) && master_of(b_z) == z && master_of(c_z) == z,
          "the mount is shared:%d; its copies are shared:%d master:%d and shared:%d master:%d", z, shared_of(b_z), master_of(b_z), shared_of(c_z), master_of(c_z));
    CHECK(umount(a_z) == 0 && !mounted(b_z) && !mounted(c_z), "unmounting under the top left a copy");

    // mount_setattr changes the same thing.
    struct { unsigned long long attr_set, attr_clr, propagation, userns_fd; } attr = {0, 0, MS_PRIVATE, 0};
    CHECK(syscall(442 /* mount_setattr */, AT_FDCWD, c, 0, &attr, sizeof(attr)) == 0, "mount_setattr with a propagation failed");
    CHECK(shared_of(c) == 0 && master_of(c) == 0, "after mount_setattr MS_PRIVATE: shared:%d master:%d", shared_of(c), master_of(c));
    attr.propagation = MS_SHARED;
    CHECK(syscall(442, AT_FDCWD, c, 0, &attr, sizeof(attr)) == 0 && shared_of(c) > 0, "mount_setattr MS_SHARED: shared:%d", shared_of(c));
    umount(c);
    umount(b);
    umount(a);
}

// pivot_root moves mounts that propagation would have to follow, and refuses
// to while it would.
static void case_pivot(void) {
    const char *a = in("pv-a");
    char root[200], old[220];
    snprintf(root, sizeof(root), "%s/root", a);
    snprintf(old, sizeof(old), "%s/root/old", a);
    CHECK(tmpfs_at(a) == 0 && mount(NULL, a, NULL, MS_SHARED, NULL) == 0, "a shared mount failed");
    CHECK(tmpfs_at(root) == 0, "mount of the new root failed");
    mkdir(old, 0755);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        CHILD_CHECK(unshare(CLONE_NEWNS) == 0, "unshare(CLONE_NEWNS) failed");
        CHILD_CHECK(shared_of(a) > 0 && shared_of(root) > 0, "the copies are shared:%d and shared:%d", shared_of(a), shared_of(root));
        errno = 0;
        CHILD_CHECK(syscall(SYS_pivot_root, root, old) == -1 && errno == EINVAL, "pivot_root under a shared mount did not fail with EINVAL");
        // The parent mount private, the new root still shared: put_old is on it.
        CHILD_CHECK(mount(NULL, a, NULL, MS_PRIVATE, NULL) == 0, "make-private failed");
        errno = 0;
        CHILD_CHECK(syscall(SYS_pivot_root, root, old) == -1 && errno == EINVAL, "pivot_root onto a shared mount did not fail with EINVAL");
        CHILD_CHECK(mount(NULL, root, NULL, MS_PRIVATE, NULL) == 0, "make-private of the new root failed");
        CHILD_CHECK(syscall(SYS_pivot_root, root, old) == 0, "pivot_root failed with neither shared");
        CHILD_CHECK(chdir("/") == 0 && exists("/old"), "after pivot_root the new root has no /old");
        _exit(0);
    }
    wait_ok(pid, "pivot_root");
    umount(root);
    umount(a);
}

// A second mount namespace, in the same user namespace: its copy of a shared
// mount is a peer.
static void case_namespace_peer(void) {
    const char *a = in("ns-a");
    char from_child[200], from_parent[200], child_only[200];
    snprintf(from_child, sizeof(from_child), "%s/from-child", a);
    snprintf(from_parent, sizeof(from_parent), "%s/from-parent", a);
    snprintf(child_only, sizeof(child_only), "%s/child-only", a);
    CHECK(tmpfs_at(a) == 0 && mount(NULL, a, NULL, MS_SHARED, NULL) == 0, "a shared mount failed");
    mkdir(from_child, 0755);
    mkdir(from_parent, 0755);
    mkdir(child_only, 0755);
    int group = shared_of(a);
    struct turn t;
    turn_init(&t);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        turn_child(&t);
        CHILD_CHECK(unshare(CLONE_NEWNS) == 0, "unshare(CLONE_NEWNS) failed");
        CHILD_CHECK(shared_of(a) == group, "the copy of a shared mount is shared:%d, the original shared:%d", shared_of(a), group);
        CHILD_CHECK(mount("c", from_child, "tmpfs", 0, NULL) == 0, "mount in the child's namespace failed");
        child_says(&t);
        child_waits(&t);
        CHILD_CHECK(mounted(from_parent), "a mount made by the parent did not appear in the child's namespace");
        // Now only receive.
        CHILD_CHECK(mount(NULL, a, NULL, MS_SLAVE, NULL) == 0, "make-slave in the child failed");
        CHILD_CHECK(master_of(a) == group && shared_of(a) == 0, "after make-slave: shared:%d master:%d", shared_of(a), master_of(a));
        CHILD_CHECK(mount("c2", child_only, "tmpfs", 0, NULL) == 0, "mount under the slave failed");
        child_says(&t);
        child_waits(&t);
        _exit(0);
    }
    turn_parent(&t);
    if (parent_waits(&t) == 0) {
        CHECK(mounted(from_child), "a mount made in the other namespace, under a peer, did not appear here");
        CHECK(mount("p", from_parent, "tmpfs", 0, NULL) == 0, "mount in the parent failed");
        parent_says(&t);
        if (parent_waits(&t) == 0)
            CHECK(!mounted(child_only), "a mount made under a slave in the other namespace appeared here");
        parent_says(&t);
    }
    turn_done(&t);
    wait_ok(pid, "a peer in another namespace");
    umount(from_child);
    umount(from_parent);
    umount(a);
}

// A new user namespace's copy is a slave from the start. What arrives in it
// is the namespace's to unmount, and stays where it came from.
static void case_namespace_user(void) {
    const char *a = in("un-a");
    char arrives[200];
    snprintf(arrives, sizeof(arrives), "%s/arrives", a);
    CHECK(tmpfs_at(a) == 0 && mount(NULL, a, NULL, MS_SHARED, NULL) == 0, "a shared mount failed");
    mkdir(arrives, 0755);
    int group = shared_of(a);
    struct turn t;
    turn_init(&t);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        turn_child(&t);
        uid_t uid = geteuid();
        gid_t gid = getegid();
        char map[64];
        CHILD_CHECK(unshare(CLONE_NEWUSER | CLONE_NEWNS) == 0, "unshare(CLONE_NEWUSER|CLONE_NEWNS) failed");
        write_file("/proc/self/setgroups", "deny");
        snprintf(map, sizeof(map), "0 %u 1", (unsigned) uid);
        CHILD_CHECK(write_file("/proc/self/uid_map", map) == 0, "uid_map");
        snprintf(map, sizeof(map), "0 %u 1", (unsigned) gid);
        CHILD_CHECK(write_file("/proc/self/gid_map", map) == 0, "gid_map");
        CHILD_CHECK(shared_of(a) == 0 && master_of(a) == group,
                    "in a new user namespace the copy is shared:%d master:%d, wanted master:%d alone", shared_of(a), master_of(a), group);
        child_says(&t);
        child_waits(&t);
        CHILD_CHECK(mounted(arrives), "a mount made outside did not arrive in the new user namespace's copy");
        CHILD_CHECK(umount(arrives) == 0, "a mount that arrived from outside could not be unmounted inside");
        CHILD_CHECK(!mounted(arrives), "it is still mounted inside");
        _exit(0);
    }
    turn_parent(&t);
    if (parent_waits(&t) == 0) {
        CHECK(mount("p", arrives, "tmpfs", 0, NULL) == 0, "mount in the parent failed");
        parent_says(&t);
    }
    turn_done(&t);
    wait_ok(pid, "a slave in a new user namespace");
    CHECK(mounted(arrives), "an unmount in the slave namespace took the mount outside with it");
    umount(arrives);
    umount(a);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    uid_t uid = geteuid();
    gid_t gid = getegid();
    if (uid != 0) {
        char map[64];
        if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
            printf(NAME ": SKIP (not root, and no user namespace to be root in: %s)\n", strerror(errno));
            return 0;
        }
        write_file("/proc/self/setgroups", "deny");
        snprintf(map, sizeof(map), "0 %u 1", (unsigned) uid);
        if (write_file("/proc/self/uid_map", map) != 0) {
            printf(NAME ": SKIP (cannot write uid_map: %s)\n", strerror(errno));
            return 0;
        }
        snprintf(map, sizeof(map), "0 %u 1", (unsigned) gid);
        write_file("/proc/self/gid_map", map);
    } else if (unshare(CLONE_NEWNS) != 0) {
        printf(NAME ": SKIP (cannot make a mount namespace: %s)\n", strerror(errno));
        return 0;
    }
    // Nothing made here is to reach the tree this one was copied from.
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        printf(NAME ": SKIP (cannot make the tree private: %s)\n", strerror(errno));
        return 0;
    }
    snprintf(base, sizeof(base), "/tmp/mount_propagation.XXXXXX");
    if (mkdtemp(base) == NULL || chmod(base, 0755) != 0 || mount("prop", base, "tmpfs", 0, NULL) != 0) {
        printf(NAME ": FAIL setup: cannot make and mount %s: %s\n", base, strerror(errno));
        rmdir(base);
        return 1;
    }

    case_shared();
    case_slave();
    case_recursive_and_refused();
    case_part();
    case_trees();
    case_chain();
    case_pivot();
    case_namespace_peer();
    case_namespace_user();

    umount2(base, MNT_DETACH);
    rmdir(base);
    if (failures != 0) {
        printf(NAME ": FAIL failures=%d\n", failures);
        return 1;
    }
    printf(NAME ": PASS\n");
    return 0;
}
