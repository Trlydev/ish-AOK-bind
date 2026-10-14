// FUSE in a user namespace: a sandbox's root may mount a filesystem served
// by its own daemon, and the conversation is in the sandbox's terms.
//
//   mount        mount(2) of "fuse" by root of a user namespace (over its own
//                mount namespace) succeeds, as fuse is FS_USERNS_MOUNT on
//                Linux; AOK refused it with EPERM
//   requests     a request's header carries its sender's fsuid and fsgid as
//                the connection's user namespace has them (0 here, for what is
//                uid 1000 outside), and its thread's id as the mounter's pid
//                namespace numbers it: 1 for the mounter, the init of a new
//                pid namespace; 0 for a sender outside that namespace
//                (fuse_fill_in_header). AOK sent kernel ids and machine pids.
//   replies      the ids a daemon gives are its namespace's: 0 is the
//                sandbox's root (uid 1000 outside), and one the namespace does
//                not map is the overflow id, 65534 (make_kuid)
//
// Speaks the raw protocol, as fuse_basic does: the daemon is a child reading
// /dev/fuse. Every expectation is Linux 6.12's, run as uid 1000. As root it
// drops to uid 1000 first; where /dev/fuse cannot be opened it skips.
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
#include <sys/wait.h>
#include <grp.h>
#include <unistd.h>

#define NAME "fuse_userns"

#define FUSE_LOOKUP 1
#define FUSE_FORGET 2
#define FUSE_GETATTR 3
#define FUSE_INIT 26
#define FUSE_INTERRUPT 36
#define FUSE_DESTROY 38
#define FUSE_BATCH_FORGET 42

struct fuse_in_header { uint32_t len, opcode; uint64_t unique, nodeid; uint32_t uid, gid, pid, padding; };
struct fuse_out_header { uint32_t len; int32_t error; uint64_t unique; };
struct fuse_wire_attr {
    uint64_t ino, size, blocks, atime, mtime, ctime;
    uint32_t atimensec, mtimensec, ctimensec;
    uint32_t mode, nlink, uid, gid, rdev, blksize, padding;
};
struct fuse_entry_out {
    uint64_t nodeid, generation, entry_valid, attr_valid;
    uint32_t entry_valid_nsec, attr_valid_nsec;
    struct fuse_wire_attr attr;
};
struct fuse_attr_out { uint64_t attr_valid; uint32_t attr_valid_nsec, dummy; struct fuse_wire_attr attr; };
struct fuse_init_in { uint32_t major, minor, max_readahead, flags; };
struct fuse_init_out {
    uint32_t major, minor, max_readahead, flags;
    uint16_t max_background, congestion_threshold;
    uint32_t max_write, time_gran;
    uint16_t max_pages, map_alignment;
    uint32_t unused[8];
};

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf(NAME ": " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static int devfd = -1, logfd = -1;

static void write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        if (write(fd, text, strlen(text)) < 0) {}
        close(fd);
    }
}

static void reply(uint64_t unique, int error, const void *body, size_t len) {
    char buf[512];
    struct fuse_out_header *out = (struct fuse_out_header *) buf;
    out->len = (uint32_t) (sizeof(*out) + len);
    out->error = error;
    out->unique = unique;
    memcpy(buf + sizeof(*out), body, len);
    if (write(devfd, buf, out->len) < 0)
        _exit(3);
}

// The root, "f" (owned by the namespace's 0) and "g" (by 4242, which it does
// not map). Nothing is cached, so each stat asks again.
static void fill(struct fuse_wire_attr *attr, uint64_t node) {
    memset(attr, 0, sizeof(*attr));
    attr->ino = node;
    attr->blksize = 4096;
    attr->nlink = node == 1 ? 2 : 1;
    attr->mode = node == 1 ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    attr->uid = attr->gid = node == 3 ? 4242 : 0;
}

static void daemon_loop(void) {
    static char buf[160 * 1024];
    for (;;) {
        ssize_t n = read(devfd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            _exit(errno == ENODEV ? 0 : 4);
        }
        struct fuse_in_header *in = (struct fuse_in_header *) buf;
        char *arg = buf + sizeof(*in);
        if (in->opcode == FUSE_INIT) {
            struct fuse_init_in *init = (void *) arg;
            struct fuse_init_out out = {.major = 7, .minor = init->minor < 31 ? init->minor : 31,
                                        .max_readahead = init->max_readahead, .max_write = 128 * 1024};
            reply(in->unique, 0, &out, sizeof(out));
        } else if (in->opcode == FUSE_LOOKUP) {
            uint64_t node = strcmp(arg, "f") == 0 ? 2 : strcmp(arg, "g") == 0 ? 3 : 0;
            char line[96];
            snprintf(line, sizeof(line), "%.15s %u %u %u\n", arg, in->uid, in->gid, in->pid);
            if (write(logfd, line, strlen(line)) < 0) {}
            if (node == 0) {
                reply(in->unique, -ENOENT, NULL, 0);
            } else {
                struct fuse_entry_out entry = {.nodeid = node};
                fill(&entry.attr, node);
                reply(in->unique, 0, &entry, sizeof(entry));
            }
        } else if (in->opcode == FUSE_GETATTR) {
            struct fuse_attr_out out = {0};
            fill(&out.attr, in->nodeid);
            reply(in->unique, 0, &out, sizeof(out));
        } else if (in->opcode == FUSE_FORGET || in->opcode == FUSE_BATCH_FORGET ||
                   in->opcode == FUSE_DESTROY || in->opcode == FUSE_INTERRUPT) {
            // (No reply.)
        } else {
            reply(in->unique, -ENOSYS, NULL, 0);
        }
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (geteuid() == 0 && (setgroups(0, NULL) != 0 || setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0)) {
        printf(NAME ": FAIL setup: cannot become uid 1000: %s\n", strerror(errno));
        return 1;
    }
    uid_t uid = geteuid();
    gid_t gid = getegid();
    char dir[] = "/tmp/fuse_userns.XXXXXX";
    if (mkdtemp(dir) == NULL) {
        printf(NAME ": FAIL setup: mkdtemp: %s\n", strerror(errno));
        return 1;
    }
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID) != 0) {
        printf(NAME ": SKIP (no user, mount and pid namespace here: %s)\n", strerror(errno));
        rmdir(dir);
        return 0;
    }
    char map[64];
    write_file("/proc/self/setgroups", "deny");
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) uid);
    write_file("/proc/self/uid_map", map);
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) gid);
    write_file("/proc/self/gid_map", map);
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    int log[2], go[2], done[2];
    if (pipe(log) != 0 || pipe(go) != 0 || pipe(done) != 0)
        return 1;
    logfd = log[1];
    pid_t mounter = fork();
    if (mounter == 0) {
        // The init of the new pid namespace: it mounts, and its daemon is 2.
        devfd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
        if (devfd < 0)
            _exit(20);
        char opts[128];
        snprintf(opts, sizeof(opts), "fd=%d,rootmode=40000,user_id=0,group_id=0", devfd);
        if (mount(NAME, dir, "fuse", MS_NOSUID | MS_NODEV, opts) != 0) {
            printf(NAME ": mount of fuse by root of a user namespace failed: %s\n", strerror(errno));
            _exit(21);
        }
        pid_t d = fork();
        if (d == 0)
            daemon_loop();
        char path[96];
        struct stat st;
        int bad = 0;
        snprintf(path, sizeof(path), "%s/f", dir);
        if (stat(path, &st) != 0 || st.st_uid != 0 || st.st_gid != 0) {
            printf(NAME ": inside, %s is %s uid %d gid %d; wanted the namespace's 0\n", path, strerror(errno), (int) st.st_uid, (int) st.st_gid);
            bad++;
        }
        snprintf(path, sizeof(path), "%s/g", dir);
        if (stat(path, &st) != 0 || st.st_uid != 65534) {
            printf(NAME ": inside, an id the namespace does not map reads %d, wanted 65534\n", (int) st.st_uid);
            bad++;
        }
        char c = 'x';
        if (write(go[1], &c, 1) != 1 || read(done[0], &c, 1) != 1)
            bad++;
        umount2(dir, MNT_DETACH);
        kill(d, SIGKILL);
        waitpid(d, NULL, 0);
        _exit(bad ? 1 : 0);
    }
    close(log[1]);
    char c;
    int ready = read(go[0], &c, 1) == 1;
    // From outside the pid namespace (the same user and mount namespace).
    char path[96];
    struct stat st;
    snprintf(path, sizeof(path), "%s/f", dir);
    if (ready)
        CHECK(stat(path, &st) == 0 && st.st_uid == 0, "outside the pid namespace, %s is %s, uid %d", path, strerror(errno), (int) st.st_uid);
    if (write(done[1], "x", 1) != 1) {}
    int status = -1;
    waitpid(mounter, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 20) {
        printf(NAME ": SKIP (cannot open /dev/fuse)\n");
        rmdir(dir);
        return 0;
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the mounter: status %#x", status);
    // What the daemon was told, one line a LOOKUP: name uid gid pid.
    char buf[1024] = "";
    size_t len = 0;
    ssize_t n;
    while (len + 1 < sizeof(buf) && (n = read(log[0], buf + len, sizeof(buf) - 1 - len)) > 0)
        len += (size_t) n;
    buf[len] = '\0';
    // The mounter's lookups of f come first (pid 1 there), then ours (0):
    // however many each stat sends -- Linux caches nothing here either, but
    // one stat need not be one lookup.
    int from_mounter = 0, from_outside = 0, out_of_order = 0;
    for (char *line = strtok(buf, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        char name[16];
        unsigned u, g, p;
        if (sscanf(line, "%15s %u %u %u", name, &u, &g, &p) != 4)
            continue;
        CHECK(u == 0 && g == 0, "a request was sent as uid %u gid %u, wanted the namespace's 0 0", u, g);
        if (strcmp(name, "f") != 0)
            continue;
        if (p == 1) {
            from_mounter++;
            out_of_order += from_outside > 0;
        } else if (p == 0) {
            from_outside++;
        } else {
            CHECK(0, "a request for f came from pid %u, which is neither the mounter's 1 nor 0 for outside", p);
        }
    }
    CHECK(from_mounter > 0, "no request for f was the mounter's (pid 1)");
    CHECK(from_outside > 0, "no request for f was from outside the pid namespace (pid 0)");
    CHECK(out_of_order == 0, "the mounter's requests came after the outside's");
    rmdir(dir);
    if (failures != 0) {
        printf(NAME ": FAIL failures=%d\n", failures);
        return 1;
    }
    printf(NAME ": PASS\n");
    return 0;
}
