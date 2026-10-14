// A descriptor number that names nothing, passed in SCM_RIGHTS, is EBADF.
//
// sendmsg() looked each number up and retained what it found without asking
// whether it had found anything, so a guest that passed a closed descriptor,
// or the -1 a failed open() had just returned, took the emulator down with a
// NULL dereference -- every session with it, on a device. (Seen as three
// suite legs dying on a host that had run out of descriptors: the guest's
// socket() had failed, and the test passed its -1 along.) Linux's
// scm_fp_copy answers EBADF and sends nothing.
//
// On each of the three AF_UNIX transports, which carry a parcel three
// different ways here (stream, datagram, seqpacket):
//
//   control      a good descriptor arrives and works: the path is the one
//                that passes descriptors at all
//   -1           EBADF, and no message is delivered
//   closed       the same for a number that was open a moment ago
//   good + bad   EBADF, and the good one is not kept: it is the write end of
//                a pipe, and once this process closes it the read end must
//                see end-of-file, which a reference left behind in the
//                kernel would prevent
//   afterwards   the socket still passes a good descriptor
//
// Prints one PASS or FAIL line.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("scm_rights_bad_fd: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

// Sends one byte with `n` descriptors attached; returns sendmsg's result
// with errno left as it set it.
static ssize_t send_fds(int sock, const int *fds, int n) {
    char byte = 'x';
    struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    char control[CMSG_SPACE(sizeof(int) * 4)];
    memset(control, 0, sizeof(control));
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = control, .msg_controllen = CMSG_SPACE(sizeof(int) * n)};
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * n);
    memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * n);
    errno = 0;
    return sendmsg(sock, &msg, MSG_NOSIGNAL);
}

// Receives one message without waiting; returns its length (or -1) and the
// first descriptor it carried, or -1.
static ssize_t recv_fd(int sock, int *fd_out) {
    char byte;
    struct iovec iov = {.iov_base = &byte, .iov_len = 1};
    char control[CMSG_SPACE(sizeof(int) * 4)];
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = control, .msg_controllen = sizeof(control)};
    *fd_out = -1;
    errno = 0;
    ssize_t n = recvmsg(sock, &msg, MSG_DONTWAIT);
    if (n < 0)
        return n;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg))
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
            memcpy(fd_out, CMSG_DATA(cmsg), sizeof(int));
    return n;
}

// A good descriptor goes across and the copy works: it is a pipe's write
// end, and what is written to the copy comes out of the pipe.
static void passes_a_good_one(const char *name, const char *when, int sv[2]) {
    int p[2];
    if (pipe(p) != 0) {
        CHECK(0, "%s: pipe: %s", name, strerror(errno));
        return;
    }
    ssize_t n = send_fds(sv[0], &p[1], 1);
    CHECK(n == 1, "%s, %s: sending a good descriptor returned %zd (%s)", name, when, n, strerror(errno));
    int got = -1;
    n = recv_fd(sv[1], &got);
    CHECK(n == 1 && got >= 0, "%s, %s: received %zd bytes and descriptor %d (%s)", name, when, n, got, strerror(errno));
    char c = 0;
    CHECK(got >= 0 && write(got, "k", 1) == 1 && read(p[0], &c, 1) == 1 && c == 'k',
          "%s, %s: the received descriptor is not the pipe that was sent", name, when);
    if (got >= 0)
        close(got);
    close(p[0]);
    close(p[1]);
}

static void refuses(const char *name, const char *what, int sv[2], const int *fds, int n) {
    ssize_t r = send_fds(sv[0], fds, n);
    int err = errno;
    CHECK(r == -1 && err == EBADF, "%s, %s: sendmsg returned %zd, errno %d (%s); wanted -1 and EBADF",
          name, what, r, err, strerror(err));
    int got = -1;
    r = recv_fd(sv[1], &got);
    err = errno;
    CHECK(r == -1 && (err == EAGAIN || err == EWOULDBLOCK),
          "%s, %s: a message was delivered all the same (%zd bytes, descriptor %d, errno %d)", name, what, r, got, err);
    if (got >= 0)
        close(got);
}

static void transport(const char *name, int type) {
    int sv[2];
    if (socketpair(AF_UNIX, type, 0, sv) != 0) {
        CHECK(0, "%s: socketpair: %s", name, strerror(errno));
        return;
    }
    passes_a_good_one(name, "the control", sv);

    int bad = -1;
    refuses(name, "-1", sv, &bad, 1);

    int closed = dup(0);
    close(closed);
    refuses(name, "a closed descriptor", sv, &closed, 1);

    int p[2];
    if (pipe(p) == 0) {
        int pair[2] = {p[1], -1};
        refuses(name, "a good one and -1", sv, pair, 2);
        close(p[1]);
        fcntl(p[0], F_SETFL, O_NONBLOCK);
        char c;
        errno = 0;
        ssize_t n = read(p[0], &c, 1);
        CHECK(n == 0, "%s: the good descriptor of a refused pair is still held: its pipe reads %zd (%s), not end-of-file",
              name, n, strerror(errno));
        close(p[0]);
    }

    passes_a_good_one(name, "after the refusals", sv);
    close(sv[0]);
    close(sv[1]);
}

int main(void) {
    transport("stream", SOCK_STREAM);
    transport("datagram", SOCK_DGRAM);
    transport("seqpacket", SOCK_SEQPACKET);
    if (failures != 0) {
        printf("scm_rights_bad_fd: FAIL failures=%d\n", failures);
        return 1;
    }
    printf("scm_rights_bad_fd: PASS\n");
    return 0;
}
