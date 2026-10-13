// What a sandbox does to a mount tree besides unsharing it: mounting over
// mounts, mount_setattr, and pivot_root(".", ".").
//
// bubblewrap got as far as its /dev on the first build with mount namespaces
// and stopped on each of these in turn.
//
//   covered      A mount made over a directory hides the mounts below it.
//                AOK's table is keyed by point, and the longer point won
//                every lookup: a tmpfs mounted over /dev came with the old
//                /dev/pts still in it ("Can't create /dev/pts: File exists").
//                Here: a mount with a file in it is hidden by a mount over
//                its parent, a descriptor held in it goes on working, the
//                name can be made afresh in the covering mount, and
//                unmounting that brings the first one back as it was.
//   rbind        A recursive bind of a directory onto itself keeps the
//                submounts in view (they are copied below it); a plain bind
//                hides them, and unmounting it shows them again.
//   mount_setattr
//                Was ENOSYS. Read-only on a bind refuses writes while its
//                source takes them; without AT_RECURSIVE a submount is left
//                alone and with it is not; clearing it restores writes; the
//                atime mode changes only as a whole field; a path that is
//                not a mount's root is EINVAL; a mount fsmount() has made
//                and nothing has placed takes attributes, and has them when
//                it is placed (systemd gives every sandboxed service its
//                /proc this way, and they all failed at step NAMESPACE when
//                this said EINVAL).
//   pivot . .    pivot_root(".", ".") then umount2(".", MNT_DETACH), the
//                form that needs no directory for the old root: afterwards
//                the new root's own mounts are all there and the old root is
//                gone. The unmount took everything below "/" with it.
//
// Needs root. On Linux, run by an ordinary user, it enters a user namespace
// first, which is how it is run against a real kernel as the oracle (6.12).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define NAME "mount_cover_setattr"
static int failures;
static char base[64];

#define CHECK(cond, ...) do { if (!(cond)) { printf(NAME ": " __VA_ARGS__); printf(" (errno %d: %s)\n", errno, strerror(errno)); failures++; } } while (0)
#define CHILD_CHECK(cond, ...) do { if (!(cond)) { printf(NAME ": " __VA_ARGS__); printf(" (errno %d: %s)\n", errno, strerror(errno)); fflush(stdout); _exit(1 + __LINE__ % 200); } } while (0)

#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442
#endif
#ifndef SYS_fsopen
#define SYS_fsopen 430
#define SYS_fsconfig 431
#define SYS_fsmount 432
#define SYS_move_mount 429
#endif
#define ATTR_RDONLY 0x1ull
#define ATTR_NOSUID 0x2ull
#define ATTR_ATIME 0x70ull
#define ATTR_NOATIME 0x10ull
#define AT_RECURSIVE_ 0x8000
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

struct attr { uint64_t set, clr, propagation, userns_fd; };

// 0, or the errno.
static int setattr(int dfd, const char *path, unsigned flags, uint64_t set, uint64_t clr) {
    struct attr a = {.set = set, .clr = clr};
    return syscall(SYS_mount_setattr, dfd, path, flags, &a, sizeof(a)) == 0 ? 0 : errno;
}

static void path_in(char *out, size_t size, const char *name) {
    snprintf(out, size, "%s/%s", base, name);
}

static int exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

// 0 if a file could be made at `path`, else the errno.
static int touch(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0)
        return errno;
    close(fd);
    return 0;
}

static int mountinfo_has(const char *point) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (f == NULL)
        return -1;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char field[5][512];
        if (sscanf(line, "%511s %511s %511s %511s %511s", field[0], field[1], field[2], field[3], field[4]) == 5 &&
                strcmp(field[4], point) == 0)
            found = 1;
    }
    fclose(f);
    return found;
}

static void case_covered(void) {
    char dir[128], sub[160], inner[200], other[200];
    path_in(dir, sizeof(dir), "cov");
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    snprintf(inner, sizeof(inner), "%s/inner", sub);
    snprintf(other, sizeof(other), "%s/other", sub);
    mkdir(dir, 0755);
    CHECK(mount("under", dir, "tmpfs", 0, NULL) == 0, "covered: mount the lower tmpfs");
    CHECK(mkdir(sub, 0755) == 0, "covered: mkdir sub");
    CHECK(mount("inner", sub, "tmpfs", 0, NULL) == 0, "covered: mount the submount");
    CHECK(touch(inner) == 0, "covered: create a file in the submount");
    int held = open(sub, O_RDONLY | O_DIRECTORY);
    CHECK(held >= 0, "covered: open the submount");

    CHECK(exists(inner), "covered: the control -- the submount's file is seen before anything covers it");
    CHECK(mount("over", dir, "tmpfs", 0, NULL) == 0, "covered: mount over the parent");
    CHECK(!exists(sub), "covered: the submount's point is still there under a fresh mount over its parent");
    CHECK(faccessat(held, "inner", F_OK, 0) == 0, "covered: a descriptor held in the covered mount lost its file");
    int made = openat(held, "made", O_WRONLY | O_CREAT, 0644);
    CHECK(made >= 0, "covered: cannot create through a descriptor held in the covered mount");
    if (made >= 0)
        close(made);
    CHECK(mkdir(sub, 0755) == 0, "covered: the name cannot be made in the covering mount");
    CHECK(touch(other) == 0, "covered: cannot create in the covering mount's directory");
    CHECK(!exists(inner), "covered: the covering mount's directory shows the covered mount's file");

    CHECK(umount(dir) == 0, "covered: unmount the covering mount");
    CHECK(exists(inner), "covered: the submount did not come back with its file");
    snprintf(other, sizeof(other), "%s/made", sub);
    CHECK(exists(other), "covered: the file made through the held descriptor is not in the submount");
    snprintf(other, sizeof(other), "%s/other", sub);
    CHECK(!exists(other), "covered: the covering mount's file is in the submount");
    if (held >= 0)
        close(held);
    CHECK(umount(sub) == 0, "covered: unmount the submount");
    CHECK(umount(dir) == 0, "covered: unmount the lower tmpfs");
    rmdir(dir);
}

static void case_rbind(void) {
    char dir[128], sub[160], inner[200];
    path_in(dir, sizeof(dir), "rb");
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    snprintf(inner, sizeof(inner), "%s/inner", sub);
    mkdir(dir, 0755);
    CHECK(mount("under", dir, "tmpfs", 0, NULL) == 0, "rbind: mount the tmpfs");
    CHECK(mkdir(sub, 0755) == 0, "rbind: mkdir sub");
    CHECK(mount("inner", sub, "tmpfs", 0, NULL) == 0, "rbind: mount the submount");
    CHECK(touch(inner) == 0, "rbind: create a file in the submount");

    CHECK(mount(dir, dir, NULL, MS_BIND | MS_REC, NULL) == 0, "rbind: recursive bind onto itself");
    CHECK(exists(inner), "rbind: a recursive bind onto itself lost the submount");
    CHECK(mount(dir, dir, NULL, MS_BIND, NULL) == 0, "rbind: plain bind onto itself");
    CHECK(exists(sub) && !exists(inner), "rbind: a plain bind onto itself still shows the submount's file");
    CHECK(umount(dir) == 0, "rbind: unmount the plain bind");
    CHECK(exists(inner), "rbind: the submount is not back after the plain bind's unmount");
    CHECK(umount2(dir, MNT_DETACH) == 0, "rbind: unmount the recursive bind");
    CHECK(exists(inner), "rbind: the original submount is not back after the recursive bind's unmount");
    CHECK(umount2(dir, MNT_DETACH) == 0, "rbind: unmount the tmpfs");
    CHECK(!exists(sub), "rbind: something is still mounted");
    rmdir(dir);
}

static void case_setattr(void) {
    char src[128], dst[128], sub[160], path[200];
    path_in(src, sizeof(src), "sa-src");
    path_in(dst, sizeof(dst), "sa-dst");
    snprintf(sub, sizeof(sub), "%s/sub", dst);
    mkdir(src, 0755);
    mkdir(dst, 0755);
    snprintf(path, sizeof(path), "%s/sub", src);
    mkdir(path, 0755);
    CHECK(mount(src, dst, NULL, MS_BIND, NULL) == 0, "setattr: bind");
    CHECK(mount("inner", sub, "tmpfs", 0, NULL) == 0, "setattr: mount the submount");

    int e = setattr(AT_FDCWD, dst, 0, 0, 0);
    if (e == ENOSYS) {
        printf(NAME ": (no mount_setattr here: that case is skipped)\n");
        umount(sub);
        umount(dst);
        return;
    }
    CHECK(e == 0, "setattr: asking for nothing returned %d", e);
    CHECK(setattr(AT_FDCWD, "/nonexistent-mount-point", 0, 0, 0) == 0, "setattr: asking for nothing of a path that is not there is not 0");
    CHECK(setattr(AT_FDCWD, "/nonexistent-mount-point", 0, ATTR_RDONLY, 0) == ENOENT, "setattr: a path that is not there is not ENOENT");
    CHECK(setattr(AT_FDCWD, src, 0, ATTR_RDONLY, 0) == EINVAL, "setattr: a directory that is not a mount's root is not EINVAL");
    CHECK(setattr(AT_FDCWD, dst, 0x4, ATTR_RDONLY, 0) == EINVAL, "setattr: an unknown flag is not EINVAL");
    CHECK(setattr(AT_FDCWD, dst, 0, 1ull << 30, 0) == EINVAL, "setattr: an unknown attribute is not EINVAL");
    CHECK(setattr(AT_FDCWD, dst, 0, ATTR_NOATIME, 0) == EINVAL, "setattr: an atime mode set without clearing the field is not EINVAL");
    CHECK(setattr(AT_FDCWD, dst, 0, 0, ATTR_NOATIME) == EINVAL, "setattr: part of the atime field cleared is not EINVAL");
    CHECK(setattr(AT_FDCWD, "", 0, ATTR_RDONLY, 0) == ENOENT, "setattr: an empty path without AT_EMPTY_PATH is not ENOENT");
    {
        struct attr a = {.set = ATTR_RDONLY};
        errno = 0;
        CHECK(syscall(SYS_mount_setattr, AT_FDCWD, dst, 0, &a, 8) == -1 && errno == EINVAL, "setattr: a struct of 8 bytes is not EINVAL");
        a.propagation = 3;
        errno = 0;
        CHECK(syscall(SYS_mount_setattr, AT_FDCWD, dst, 0, &a, sizeof(a)) == -1 && errno == EINVAL, "setattr: a propagation that is not one is not EINVAL");
        char big[64] = {0};
        memcpy(big, &(struct attr) {.set = ATTR_NOSUID}, sizeof(struct attr));
        errno = 0;
        CHECK(syscall(SYS_mount_setattr, AT_FDCWD, dst, 0, big, 40) == 0, "setattr: a larger struct whose tail is zero is refused");
        big[36] = 1;
        errno = 0;
        CHECK(syscall(SYS_mount_setattr, AT_FDCWD, dst, 0, big, 40) == -1 && errno == E2BIG, "setattr: a larger struct with a nonzero tail is not E2BIG");
    }
    CHECK(setattr(AT_FDCWD, dst, 0, ATTR_NOATIME, ATTR_ATIME) == 0, "setattr: noatime, the field cleared first");

    snprintf(path, sizeof(path), "%s/a", dst);
    CHECK(touch(path) == 0, "setattr: the control -- the bind takes a write before it is read-only");
    CHECK(setattr(AT_FDCWD, dst, 0, ATTR_RDONLY, 0) == 0, "setattr: read-only");
    snprintf(path, sizeof(path), "%s/b", dst);
    CHECK(touch(path) == EROFS, "setattr: a write through the read-only bind was not EROFS");
    snprintf(path, sizeof(path), "%s/b", src);
    CHECK(touch(path) == 0, "setattr: the bind's source stopped taking writes");
    snprintf(path, sizeof(path), "%s/c", sub);
    CHECK(touch(path) == 0, "setattr: without AT_RECURSIVE the submount went read-only too");
    CHECK(setattr(AT_FDCWD, dst, AT_RECURSIVE_, ATTR_RDONLY, 0) == 0, "setattr: read-only, recursive");
    snprintf(path, sizeof(path), "%s/d", sub);
    CHECK(touch(path) == EROFS, "setattr: with AT_RECURSIVE the submount still takes writes");
    int fd = open(dst, O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 0 && setattr(fd, "", AT_EMPTY_PATH | AT_RECURSIVE_, 0, ATTR_RDONLY) == 0, "setattr: clear read-only by descriptor");
    if (fd >= 0)
        close(fd);
    snprintf(path, sizeof(path), "%s/e", dst);
    CHECK(touch(path) == 0, "setattr: the bind is still read-only after the clear");
    snprintf(path, sizeof(path), "%s/e", sub);
    CHECK(touch(path) == 0, "setattr: the submount is still read-only after the recursive clear");
    CHECK(umount(sub) == 0, "setattr: unmount the submount");
    CHECK(umount(dst) == 0, "setattr: unmount the bind");

    // A mount no tree has yet.
    int fsfd = (int) syscall(SYS_fsopen, "tmpfs", 0);
    if (fsfd < 0) {
        printf(NAME ": (no fsopen here: the detached-mount check is skipped)\n");
        return;
    }
    CHECK(syscall(SYS_fsconfig, fsfd, 6 /* FSCONFIG_CMD_CREATE */, NULL, NULL, 0) == 0, "setattr: fsconfig(CREATE)");
    int mfd = (int) syscall(SYS_fsmount, fsfd, 0, 0);
    CHECK(mfd >= 0, "setattr: fsmount");
    close(fsfd);
    if (mfd >= 0) {
        CHECK(setattr(mfd, "", AT_EMPTY_PATH, ATTR_RDONLY, 0) == 0, "setattr: a mount fsmount made and nothing placed refuses attributes");
        CHECK(syscall(SYS_move_mount, mfd, "", AT_FDCWD, dst, 4 /* MOVE_MOUNT_F_EMPTY_PATH */) == 0, "setattr: move_mount");
        snprintf(path, sizeof(path), "%s/f", dst);
        CHECK(touch(path) == EROFS, "setattr: the placed mount does not have the read-only it was given");
        close(mfd);
        CHECK(umount(dst) == 0, "setattr: unmount the placed mount");
    }
    rmdir(sub);
}

static void case_pivot_stacked(void) {
    char newroot[128];
    path_in(newroot, sizeof(newroot), "newroot");
    mkdir(newroot, 0755);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        char path[200];
        CHILD_CHECK(unshare(CLONE_NEWNS) == 0, "pivot: unshare(CLONE_NEWNS) failed");
        CHILD_CHECK(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0, "pivot: making the tree private failed");
        CHILD_CHECK(mount("root", newroot, "tmpfs", 0, NULL) == 0, "pivot: mount tmpfs for the new root failed");
        snprintf(path, sizeof(path), "%s/top", newroot);
        CHILD_CHECK(touch(path) == 0, "pivot: cannot create a file in the new root");
        snprintf(path, sizeof(path), "%s/sub", newroot);
        CHILD_CHECK(mkdir(path, 0755) == 0 && mount("sub", path, "tmpfs", 0, NULL) == 0, "pivot: cannot mount in the new root");
        snprintf(path, sizeof(path), "%s/sub/marker", newroot);
        CHILD_CHECK(touch(path) == 0, "pivot: cannot create a file in the new root's mount");
        snprintf(path, sizeof(path), "%s/proc", newroot);
        CHILD_CHECK(mkdir(path, 0755) == 0 && mount("/proc", path, NULL, MS_BIND | MS_REC, NULL) == 0, "pivot: cannot bind /proc into the new root");

        int oldroot = open("/", O_RDONLY | O_DIRECTORY);
        CHILD_CHECK(oldroot >= 0, "pivot: open the old root");
        CHILD_CHECK(chdir(newroot) == 0, "pivot: chdir to the new root");
        CHILD_CHECK(syscall(SYS_pivot_root, ".", ".") == 0, "pivot: pivot_root(\".\", \".\") failed");
        CHILD_CHECK(fchdir(oldroot) == 0, "pivot: fchdir to the old root");
        CHILD_CHECK(umount2(".", MNT_DETACH) == 0, "pivot: unmounting the old root failed");
        CHILD_CHECK(chdir("/") == 0, "pivot: chdir(\"/\")");
        close(oldroot);
        CHILD_CHECK(exists("/top"), "pivot: the new root's file is not at /top");
        CHILD_CHECK(!exists("/etc"), "pivot: the old root's /etc is still there");
        CHILD_CHECK(exists("/sub/marker"), "pivot: the mount in the new root went with the old root");
        CHILD_CHECK(mountinfo_has("/sub") == 1, "pivot: /proc/self/mountinfo does not list the new root's mount");
        CHILD_CHECK(mountinfo_has("/") == 1, "pivot: /proc/self/mountinfo has no root");
        _exit(0);
    }
    int status = -1;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf(NAME ": pivot: child status %#x\n", status);
        failures++;
    }
    char path[200];
    snprintf(path, sizeof(path), "%s/top", newroot);
    errno = 0;
    CHECK(!exists(path), "pivot: the parent sees the child's new root");
    CHECK(exists("/etc"), "pivot: the parent's own root changed");
    rmdir(newroot);
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
    if (geteuid() != 0) {
#ifdef __linux__
        uid_t uid = getuid();
        gid_t gid = getgid();
        char map[64];
        if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
            printf(NAME ": SKIP (not root, and no user namespace to be root in: %s)\n", strerror(errno));
            return 0;
        }
        write_file("/proc/self/setgroups", "deny");
        snprintf(map, sizeof(map), "0 %d 1", (int) uid);
        if (write_file("/proc/self/uid_map", map) != 0) {
            printf(NAME ": SKIP (cannot write uid_map: %s)\n", strerror(errno));
            return 0;
        }
        snprintf(map, sizeof(map), "0 %d 1", (int) gid);
        write_file("/proc/self/gid_map", map);
        if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
            printf(NAME ": SKIP (cannot make the tree private: %s)\n", strerror(errno));
            return 0;
        }
#else
        printf(NAME ": SKIP (needs root)\n");
        return 0;
#endif
    }
    snprintf(base, sizeof(base), "/tmp/mount_cover.XXXXXX");
    if (mkdtemp(base) == NULL || chmod(base, 0755) != 0 || mount("cover", base, "tmpfs", 0, NULL) != 0) {
        printf(NAME ": FAIL setup: cannot make and mount %s: %s\n", base, strerror(errno));
        rmdir(base);
        return 1;
    }
    errno = 0;

    case_covered();
    case_rbind();
    case_setattr();
    case_pivot_stacked();

    umount2(base, MNT_DETACH);
    rmdir(base);
    if (failures != 0) {
        printf(NAME ": FAIL failures=%d\n", failures);
        return 1;
    }
    printf(NAME ": PASS\n");
    return 0;
}
