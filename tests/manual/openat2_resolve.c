// openat2's RESOLVE_* constraints.
//
//   Every RESOLVE_* bit was rejected with EINVAL, so the whole reason openat2
//   exists over openat -- constraining how the path is allowed to resolve --
//   was unusable. RESOLVE_NO_SYMLINKS is the one that matters most in
//   practice: it is how a program opens a path it does not control without a
//   symlink somewhere in it redirecting the open elsewhere.
//
//   AOK implements NO_SYMLINKS, NO_MAGICLINKS and IN_ROOT inside path
//   resolution, and answers CACHED with EAGAIN. IN_ROOT is "as if the caller
//   had chroot()ed to dirfd", and is the walk that confines a chrooted task
//   with dirfd as its root (bubblewrap opens every bind source this way, from
//   the old root it has pivoted away from). RESOLVE_BENEATH and
//   RESOLVE_NO_XDEV stay refused with EINVAL, which is what a kernel without
//   them says and what every caller already handles -- openat2 is Linux 5.6+,
//   so nothing may assume it. This test accepts EITHER a working
//   implementation or EINVAL for those two, and pins the rest.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "test_common.h"

#define RESOLVE_NO_XDEV_       0x01
#define RESOLVE_NO_MAGICLINKS_ 0x02
#define RESOLVE_NO_SYMLINKS_   0x04
#define RESOLVE_BENEATH_       0x08
#define RESOLVE_IN_ROOT_       0x10
#define RESOLVE_CACHED_        0x20

struct open_how_ {
    unsigned long long flags, mode, resolve;
};

static char base[80];

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-8ld want=%ld\n", label, got, want);
}

// The fd on success, or -errno.
static long open2(int dfd, const char *path, unsigned long long resolve) {
    struct open_how_ how;
    memset(&how, 0, sizeof how);
    how.flags = O_RDONLY;
    how.resolve = resolve;
    errno = 0;
    long r = syscall(SYS_openat2, dfd, path, &how, sizeof how);
    return r < 0 ? -errno : r;
}

static void close_if_fd(long r) {
    if (r >= 0)
        close((int) r);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    snprintf(base, sizeof base, "/tmp/openat2-resolve-%d", (int) getpid());
    char cmd[200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) < 0)
        failures_total++;
    ck("mkdir base", mkdir(base, 0755), 0);

    char sub[160], file[160], link[160], vialink[220], dirlink[160];
    snprintf(sub, sizeof sub, "%s/sub", base);
    snprintf(file, sizeof file, "%s/sub/f", base);
    snprintf(link, sizeof link, "%s/link", base);
    snprintf(dirlink, sizeof dirlink, "%s/dirlink", base);
    snprintf(vialink, sizeof vialink, "%s/dirlink/f", base);
    ck("mkdir sub", mkdir(sub, 0755), 0);
    {
        int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ck("create the target file", fd >= 0, 1);
        if (fd >= 0)
            close(fd);
    }
    ck("symlink to the file", symlink(file, link), 0);
    ck("symlink to the directory", symlink(sub, dirlink), 0);

    int dfd = open(base, O_RDONLY | O_DIRECTORY);
    ck("open the base directory", dfd >= 0, 1);
    if (dfd < 0)
        return finish_suite("openat2_resolve");

    // ---- openat2 works at all ---------------------------------------------
    {
        long r = open2(dfd, "sub/f", 0);
        ck("openat2 with no constraints opens the file", r >= 0, 1);
        close_if_fd(r);
    }

    // ---- RESOLVE_NO_SYMLINKS ----------------------------------------------
    {
        // A path with no symlink in it is unaffected -- the constraint must not
        // simply fail everything.
        long r = open2(dfd, "sub/f", RESOLVE_NO_SYMLINKS_);
        ck("NO_SYMLINKS on a symlink-free path still opens", r >= 0, 1);
        close_if_fd(r);

        // The final component is a symlink.
        ck("NO_SYMLINKS on a symlink is ELOOP",
           open2(dfd, "link", RESOLVE_NO_SYMLINKS_), -ELOOP);
        // ...and so is one in the MIDDLE, which is the case that matters: the
        // caller controls the last name and not the directories above it.
        ck("NO_SYMLINKS through a symlinked directory is ELOOP",
           open2(dfd, "dirlink/f", RESOLVE_NO_SYMLINKS_), -ELOOP);

        // Without the flag both still resolve, so the failures above are the
        // constraint and not a broken path.
        long a = open2(dfd, "link", 0);
        ck("...but without the flag the symlink opens", a >= 0, 1);
        close_if_fd(a);
        long b = open2(dfd, "dirlink/f", 0);
        ck("...and so does the symlinked directory", b >= 0, 1);
        close_if_fd(b);
    }

    // ---- RESOLVE_CACHED ----------------------------------------------------
    // "Only if this is already cached, else EAGAIN." On Linux the answer
    // depends on what happens to be in the dcache, so a caller cannot rely on
    // either -- which is exactly why declining is honest. Accept either.
    {
        long r = open2(dfd, "sub/f", RESOLVE_CACHED_);
        test_logf("  %-56s got=%ld\n", "CACHED answers", r);
        ck("CACHED either opens or is EAGAIN", r >= 0 || r == -EAGAIN, 1);
        close_if_fd(r);
    }

    // ---- RESOLVE_IN_ROOT ----------------------------------------------------
    // dirfd is the root of the walk. Each case says which file it reached, by
    // inode, so that "it opened something" cannot pass for "it stayed inside".
    {
        struct stat want_f, want_base, got;
        ck("stat the file", stat(file, &want_f), 0);
        ck("stat the base", stat(base, &want_base), 0);
#define REACHES(label, path, resolve, want) do { \
            long r_ = open2(dfd, path, resolve); \
            ck(label, r_ >= 0 && fstat((int) r_, &got) == 0 && \
               got.st_ino == (want).st_ino && got.st_dev == (want).st_dev, 1); \
            close_if_fd(r_); \
        } while (0)
        REACHES("IN_ROOT: a relative path opens", "sub/f", RESOLVE_IN_ROOT_, want_f);
        REACHES("IN_ROOT: an absolute path starts at dirfd", "/sub/f", RESOLVE_IN_ROOT_, want_f);
        REACHES("IN_ROOT: \"/\" is dirfd itself", "/", RESOLVE_IN_ROOT_, want_base);
        REACHES("IN_ROOT: \"..\" stays at dirfd", "..", RESOLVE_IN_ROOT_, want_base);
        REACHES("IN_ROOT: \"..\" from below climbs to dirfd and stops", "sub/../../../sub/f",
                RESOLVE_IN_ROOT_, want_f);

        // An absolute link is walked from dirfd. "inlink" -> /sub/f names
        // nothing from the real root and the file from this one; "outlink" ->
        // /etc/passwd is the other way round.
        char inlink[160], outlink[160], uplink[160];
        snprintf(inlink, sizeof inlink, "%s/inlink", base);
        snprintf(outlink, sizeof outlink, "%s/outlink", base);
        snprintf(uplink, sizeof uplink, "%s/sub/uplink", base);
        ck("symlink inlink -> /sub/f", symlink("/sub/f", inlink), 0);
        ck("symlink outlink -> /etc/passwd", symlink("/etc/passwd", outlink), 0);
        ck("symlink sub/uplink -> ../../../sub/f", symlink("../../../sub/f", uplink), 0);
        REACHES("IN_ROOT: an absolute link is walked from dirfd", "inlink", RESOLVE_IN_ROOT_, want_f);
        ck("...which without the flag names nothing", open2(dfd, "inlink", 0), -ENOENT);
        ck("IN_ROOT: a link out of the root does not leave it",
           open2(dfd, "outlink", RESOLVE_IN_ROOT_), -ENOENT);
        long out = open2(dfd, "outlink", 0);
        ck("...which without the flag opens /etc/passwd", out >= 0, 1);
        close_if_fd(out);
        REACHES("IN_ROOT: a link's \"..\" stops at dirfd too", "sub/uplink", RESOLVE_IN_ROOT_, want_f);
        // The base's own absolute links, made above, now name nothing.
        ck("IN_ROOT: a link spelled from the real root is ENOENT",
           open2(dfd, "link", RESOLVE_IN_ROOT_), -ENOENT);
        ck("IN_ROOT: a dirfd that is not a directory is ENOTDIR", ({
            int ffd = open(file, O_RDONLY);
            long r = open2(ffd, "/", RESOLVE_IN_ROOT_);
            close_if_fd(r);
            close(ffd);
            r;
        }), -ENOTDIR);
        ck("IN_ROOT: a bad dirfd is EBADF", open2(987, "/", RESOLVE_IN_ROOT_), -EBADF);
#undef REACHES
        unlink(inlink);
        unlink(outlink);
        unlink(uplink);
    }

    // ---- RESOLVE_NO_MAGICLINKS, and magic links under IN_ROOT ---------------
    // A link under /proc/<pid>/ names a file rather than spelling a path.
    // /proc/self is an ordinary link.
    {
        int rootfd = open("/", O_RDONLY | O_DIRECTORY);
        ck("open /", rootfd >= 0, 1);
        char viafd[64], through[96];
        snprintf(viafd, sizeof viafd, "proc/self/fd/%d", dfd);
        snprintf(through, sizeof through, "proc/self/fd/%d/sub/f", dfd);
        long r = open2(rootfd, "proc/self/status", 0);
        if (r < 0) {
            test_logf("  (no /proc here: the magic-link cases are skipped)\n");
        } else {
            close((int) r);
            r = open2(rootfd, through, 0);
            ck("without a flag a path through /proc/self/fd/N opens", r >= 0, 1);
            close_if_fd(r);
            r = open2(rootfd, "proc/self/status", RESOLVE_NO_MAGICLINKS_);
            ck("NO_MAGICLINKS follows /proc/self, an ordinary link", r >= 0, 1);
            close_if_fd(r);
            ck("NO_MAGICLINKS on /proc/self/fd/N is ELOOP",
               open2(rootfd, viafd, RESOLVE_NO_MAGICLINKS_), -ELOOP);
            ck("NO_MAGICLINKS through /proc/self/fd/N is ELOOP",
               open2(rootfd, through, RESOLVE_NO_MAGICLINKS_), -ELOOP);
            ck("NO_MAGICLINKS on /proc/self/cwd is ELOOP",
               open2(rootfd, "proc/self/cwd", RESOLVE_NO_MAGICLINKS_), -ELOOP);
            ck("NO_SYMLINKS on /proc/self/fd/N is ELOOP",
               open2(rootfd, viafd, RESOLVE_NO_SYMLINKS_), -ELOOP);
            r = open2(rootfd, "/proc/self/status", RESOLVE_IN_ROOT_);
            ck("IN_ROOT follows /proc/self", r >= 0, 1);
            close_if_fd(r);
            ck("IN_ROOT on /proc/self/fd/N is EXDEV",
               open2(rootfd, viafd, RESOLVE_IN_ROOT_), -EXDEV);
            ck("IN_ROOT through /proc/self/fd/N is EXDEV",
               open2(rootfd, through, RESOLVE_IN_ROOT_), -EXDEV);
            ck("IN_ROOT with NO_MAGICLINKS is ELOOP",
               open2(rootfd, viafd, RESOLVE_IN_ROOT_ | RESOLVE_NO_MAGICLINKS_), -ELOOP);
        }
        close(rootfd);
    }

    // ---- the sandbox flags: implemented, or honestly refused ---------------
    {
        struct { const char *name; unsigned long long bit; } sandbox[] = {
            { "BENEATH", RESOLVE_BENEATH_ },
            { "IN_ROOT", RESOLVE_IN_ROOT_ },
            { "NO_XDEV", RESOLVE_NO_XDEV_ },
            { "NO_MAGICLINKS", RESOLVE_NO_MAGICLINKS_ },
        };
        for (unsigned i = 0; i < sizeof sandbox / sizeof sandbox[0]; i++) {
            long r = open2(dfd, "sub/f", sandbox[i].bit);
            char label[96];
            snprintf(label, sizeof label, "%s works or is EINVAL", sandbox[i].name);
            ck(label, r >= 0 || r == -EINVAL, 1);
            // What must not happen is succeeding without enforcing: if it
            // opened, escaping must be refused.
            if (r >= 0 && sandbox[i].bit == RESOLVE_BENEATH_) {
                close((int) r);
                long esc = open2(dfd, "../", RESOLVE_BENEATH_);
                ck("  ...and if BENEATH works, escaping is refused", esc < 0, 1);
                close_if_fd(esc);
            } else {
                close_if_fd(r);
            }
        }
    }

    // ---- an unknown resolve bit is EINVAL ---------------------------------
    ck("an unknown RESOLVE bit is EINVAL", open2(dfd, "sub/f", 0x8000), -EINVAL);

    close(dfd);
    if (system(cmd) < 0)
        failures_total++;
    return finish_suite("openat2_resolve");
}
