// What an O_PATH open keeps of the flags it is given, and what fcntl answers
// on the descriptor it makes.
//
// O_PATH beats everything else (Linux's build_open_how): the open performs no
// permission check, so it keeps only O_PATH, O_DIRECTORY, O_NOFOLLOW and
// O_CLOEXEC, and does nothing the check would have guarded. AOK skipped the
// check and kept the rest: open(f, O_PATH|O_WRONLY|O_TRUNC) by an ordinary
// user emptied a root-owned 0644 file. O_CREAT is dropped too -- a missing
// name is ENOENT and nothing is made. openat2 is strict instead: O_PATH with
// any other flag is EINVAL.
//
// The descriptor is a location, not an open file. F_GETFL is O_PATH and the
// lookup flags it was opened with, never an access mode (AOK reported the
// host's read-only open); fcntl answers F_DUPFD, F_DUPFD_CLOEXEC, F_GETFD,
// F_SETFD and F_GETFL, and everything else -- F_SETFL, locks, owners, seals
// -- is EBADF (check_fcntl_cmd). AOK went on to act on it.
//
// Nor does it open a device: it names the node. /dev/tty in a session with no
// controlling terminal is ENXIO to an open and a handle to O_PATH, with the
// node's type. AOK opened the device either way, and bubblewrap -- which
// binds /dev/tty into every sandbox's /dev from such a handle -- stopped
// there whenever it was run without a terminal.
//
// Run as root the truncation cases are run again as uid 65534 in a child.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32, as root and as uid 1000.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#endif
#ifndef SYS_openat2
#define SYS_openat2 437
#endif

struct how {
    uint64_t flags, mode, resolve;
};

static void check(int cond, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!cond) {
        printf("FAIL ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    } else if (test_verbose) {
        printf("ok ");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
}

static const char *dir = "/tmp/opath_open_flags.d";
static char file[128], missing[128];

static long size_of(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long) st.st_size : -1;
}

// The flags that act on the file do nothing under O_PATH.
static void truncation(const char *who) {
    static const struct { const char *name; int flags; } cases[] = {
        {"O_PATH|O_WRONLY|O_TRUNC", O_PATH | O_WRONLY | O_TRUNC},
        {"O_PATH|O_RDWR|O_TRUNC", O_PATH | O_RDWR | O_TRUNC},
        {"O_PATH|O_TRUNC|O_CREAT", O_PATH | O_TRUNC | O_CREAT},
        {"O_PATH|O_WRONLY|O_APPEND", O_PATH | O_WRONLY | O_APPEND},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int fd = open(file, cases[i].flags | O_CLOEXEC, 0644);
        check(fd >= 0, "%s: %s opens (%s)", who, cases[i].name, strerror(errno));
        check(size_of(file) == 5, "%s: %s leaves the file its 5 bytes (size %ld)", who,
              cases[i].name, size_of(file));
        if (fd < 0)
            continue;
        int fl = fcntl(fd, F_GETFL);
        check(fl == O_PATH, "%s: %s: F_GETFL is O_PATH (%#x)", who, cases[i].name, fl);
        errno = 0;
        check(write(fd, "x", 1) < 0 && errno == EBADF, "%s: %s: write is EBADF (%s)", who,
              cases[i].name, strerror(errno));
        close(fd);
    }
    errno = 0;
    int fd = open(missing, O_PATH | O_CREAT | O_CLOEXEC, 0644);
    check(fd < 0 && errno == ENOENT, "%s: O_PATH|O_CREAT of a missing name is ENOENT (%s)", who,
          fd >= 0 ? "opened" : strerror(errno));
    check(access(missing, F_OK) != 0, "%s:   and makes nothing", who);
    if (fd >= 0)
        close(fd);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));
    snprintf(file, sizeof(file), "%s/file", dir);
    snprintf(missing, sizeof(missing), "%s/missing", dir);
    unlink(file);
    unlink(missing);
    rmdir(dir);
    check(mkdir(dir, 0755) == 0, "mkdir %s (%s)", dir, strerror(errno));
    int fd = open(file, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
    check(fd >= 0 && write(fd, "hello", 5) == 5, "create %s (%s)", file, strerror(errno));
    if (fd >= 0)
        close(fd);

    truncation(geteuid() == 0 ? "root" : "unprivileged");
    if (geteuid() == 0) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            if (setgid(65534) != 0 || setuid(65534) != 0) {
                printf("FAIL cannot drop to uid 65534 (%s)\n", strerror(errno));
                _exit(1);
            }
            truncation("uid 65534");
            fflush(stdout);
            _exit(failures_total != 0);
        }
        int status = 0;
        check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "uid 65534's cases pass (status %#x)", status);
    }

    // F_GETFL keeps the lookup flags, and nothing but them.
    int d = open(dir, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    check(d >= 0, "O_PATH|O_DIRECTORY|O_NOFOLLOW of %s (%s)", dir, strerror(errno));
    if (d >= 0) {
        int fl = fcntl(d, F_GETFL);
        check(fl == (O_PATH | O_DIRECTORY | O_NOFOLLOW),
              "F_GETFL is O_PATH|O_DIRECTORY|O_NOFOLLOW, %#x (got %#x)",
              O_PATH | O_DIRECTORY | O_NOFOLLOW, fl);

        // fcntl: what is about the descriptor, and nothing else.
        int dup = fcntl(d, F_DUPFD, 20);
        check(dup >= 20, "F_DUPFD works (%s)", strerror(errno));
        if (dup >= 0)
            close(dup);
        dup = fcntl(d, F_DUPFD_CLOEXEC, 20);
        check(dup >= 20, "F_DUPFD_CLOEXEC works (%s)", strerror(errno));
        if (dup >= 0)
            close(dup);
        check(fcntl(d, F_SETFD, 0) == 0 && fcntl(d, F_GETFD) == 0, "F_SETFD and F_GETFD work");
        struct flock lk = {.l_type = F_RDLCK, .l_whence = SEEK_SET};
        struct { const char *name; int cmd; long arg; } refused[] = {
            {"F_SETFL", F_SETFL, O_NONBLOCK},
            {"F_GETOWN", F_GETOWN, 0},
            {"F_SETOWN", F_SETOWN, 0},
            {"F_GETLK", F_GETLK, (long) &lk},
            {"F_SETLK", F_SETLK, (long) &lk},
            {"F_GET_SEALS", F_GET_SEALS, 0},
        };
        for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
            errno = 0;
            int r = fcntl(d, refused[i].cmd, refused[i].arg);
            check(r < 0 && errno == EBADF, "%s is EBADF (%d, %s)", refused[i].name, r,
                  strerror(errno));
        }
        close(d);
    }

    // openat2: O_PATH with a flag it does not keep is EINVAL, not dropped.
    struct how how = {.flags = O_PATH | O_RDWR};
    errno = 0;
    long r = syscall(SYS_openat2, AT_FDCWD, file, &how, sizeof(how));
    if (r < 0 && errno == ENOSYS) {
        test_logf("openat2 unsupported here\n");
    } else {
        check(r < 0 && errno == EINVAL, "openat2 O_PATH|O_RDWR is EINVAL (%ld, %s)", r,
              strerror(errno));
        if (r >= 0)
            close((int) r);
        how.flags = O_PATH | O_TRUNC;
        errno = 0;
        r = syscall(SYS_openat2, AT_FDCWD, file, &how, sizeof(how));
        check(r < 0 && errno == EINVAL, "openat2 O_PATH|O_TRUNC is EINVAL (%ld, %s)", r,
              strerror(errno));
        if (r >= 0)
            close((int) r);
        how.flags = O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
        r = syscall(SYS_openat2, AT_FDCWD, dir, &how, sizeof(how));
        check(r >= 0, "openat2 O_PATH with the flags it keeps opens (%s)", strerror(errno));
        if (r >= 0)
            close((int) r);
    }
    check(size_of(file) == 5, "the file still has its 5 bytes (size %ld)", size_of(file));

    // A device that will not open. The child has a session of its own, so
    // no controlling terminal; its exit status says what it found.
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        if (setsid() < 0)
            _exit(10);
        struct stat st;
        if (stat("/dev/tty", &st) != 0 || !S_ISCHR(st.st_mode))
            _exit(11); // no /dev/tty here to ask about
        int fd = open("/dev/tty", O_RDONLY | O_NOCTTY);
        if (fd >= 0 || errno != ENXIO)
            _exit(12); // the control: it must be a device that refuses
        fd = open("/dev/tty", O_PATH);
        if (fd < 0)
            _exit(errno == ENXIO ? 13 : 14);
        if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode))
            _exit(15);
        char c;
        errno = 0;
        if (read(fd, &c, 1) != -1 || errno != EBADF)
            _exit(16);
        _exit(0);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 11 || code == 12)
        test_logf("/dev/tty is absent or opens with no controlling terminal (%d): not checked\n", code);
    else
        check(code == 0, "O_PATH on /dev/tty with no controlling terminal: %s (%d)",
              code == 13 ? "ENXIO, the device's own answer" :
              code == 14 ? "another error" :
              code == 15 ? "not a character device to fstat" :
              code == 16 ? "the handle reads" : code == 0 ? "a handle" : "the child died", code);

    unlink(file);
    rmdir(dir);
    return finish_suite("opath_open_flags");
}
