// Guest half of tcp_suspend_reset.sh: what a connected TCP socket tells its
// owner after an iOS suspension has taken the connection away.
//
// iOS defuncts a suspended app's TCP connections once the device locks. XNU
// leaves EBADF as the pending error of a defunct socket -- on a descriptor
// that is perfectly good -- and AOK reads the pending error itself whenever
// the guest polls a stream socket, keeping it for the guest's next call. So a
// client that sat in poll() across the suspension, which is every ssh
// session, got "Read from remote host: Bad file descriptor" (GH #631), where
// Linux reports a connection the network took away as reset. A program that
// reconnects on a reset does not on EBADF: that one means its own bug.
//
// The host script runs this on a Mac CLI build with ISH_SOCKRESTART_AFTER=1
// and ISH_SOCKRESTART_TEST_DESTROY=defunct, which does to this process's
// sockets what a suspension does. A server thread and six client connections
// are set up first; after the cycle each connection is asked one question,
// because a pending error belongs to whoever asks first:
//
//   poll-recv   poll(), then recv(): the reported shape
//   poll-so     poll(), then getsockopt(SO_ERROR)
//   so          getsockopt(SO_ERROR) alone
//   recv        recv() alone
//   send        send() alone
//   epoll-read  epoll_wait(), then read()
//
// Each must fail with ECONNRESET (EPIPE also for send), none with EBADF, and
// the cycle must really have happened: every poll must report the connection
// readable or hung up, which a live idle one is not. Without the simulated
// suspension every connection is alive and the test says so and fails -- it
// cannot pass because nothing happened.
//
// Prints "PASS" or "FAIL ..." lines and exits 0 or 1.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static int listener;

static void *serve(void *unused) {
    (void) unused;
    for (;;) {
        int c = accept(listener, NULL, NULL);
        if (c < 0)
            return NULL;
        // Say hello, so the client knows the connection is whole, and keep it.
        (void) !write(c, "h", 1);
    }
}

static int connect_one(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    char hello;
    if (s < 0 || connect(s, (struct sockaddr *) &addr, sizeof(addr)) != 0 || recv(s, &hello, 1, 0) != 1) {
        printf("FAIL setup: connect: %s\n", strerror(errno));
        _exit(1);
    }
    return s;
}

// The connection must look gone to poll: readable (an error or an end to
// read) or hung up. A live, idle connection is neither.
static void poll_gone(const char *name, int s) {
    struct pollfd p = {.fd = s, .events = POLLIN};
    int n = poll(&p, 1, 0);
    CHECK(n == 1 && (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0,
          "%s: poll says the connection is still alive (%d, revents %#x): no suspension was simulated?",
          name, n, p.revents);
}

static void reset_expected(const char *name, const char *call, long res, int err, int also) {
    CHECK(res < 0 && (err == ECONNRESET || (also != 0 && err == also)),
          "%s: %s returned %ld, errno %d (%s); wanted ECONNRESET", name, call, res, err, strerror(err));
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET};
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    socklen_t len = sizeof(addr);
    if (listener < 0 || bind(listener, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
        listen(listener, 16) != 0 || getsockname(listener, (struct sockaddr *) &addr, &len) != 0) {
        printf("FAIL setup: listen: %s\n", strerror(errno));
        return 1;
    }
    pthread_t server;
    pthread_create(&server, NULL, serve, NULL);
    int port = ntohs(addr.sin_port);
    int s[6];
    for (int i = 0; i < 6; i++)
        s[i] = connect_one(port);

    // The suspension and the resume happen in here (one second in, half a
    // second long).
    sleep(3);

    char buf[8];
    long res;
    int err;

    poll_gone("poll-recv", s[0]);
    errno = 0; res = recv(s[0], buf, sizeof(buf), MSG_DONTWAIT); err = errno;
    reset_expected("poll-recv", "recv", res, err, 0);

    poll_gone("poll-so", s[1]);
    int pending = -1;
    len = sizeof(pending);
    res = getsockopt(s[1], SOL_SOCKET, SO_ERROR, &pending, &len);
    CHECK(res == 0 && pending == ECONNRESET, "poll-so: SO_ERROR is %d (%s); wanted ECONNRESET", pending, strerror(pending));

    pending = -1;
    len = sizeof(pending);
    res = getsockopt(s[2], SOL_SOCKET, SO_ERROR, &pending, &len);
    CHECK(res == 0 && pending == ECONNRESET, "so: SO_ERROR is %d (%s); wanted ECONNRESET", pending, strerror(pending));
    // ...and it was the one report of it.
    pending = -1;
    res = getsockopt(s[2], SOL_SOCKET, SO_ERROR, &pending, &len);
    CHECK(res == 0 && pending == 0, "so: SO_ERROR a second time is %d (%s); wanted 0", pending, strerror(pending));

    errno = 0; res = recv(s[3], buf, sizeof(buf), MSG_DONTWAIT); err = errno;
    reset_expected("recv", "recv", res, err, 0);

    errno = 0; res = send(s[4], "x", 1, 0); err = errno;
    reset_expected("send", "send", res, err, EPIPE);

    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = s[5]}, got;
    epoll_ctl(ep, EPOLL_CTL_ADD, s[5], &ev);
    int n = epoll_wait(ep, &got, 1, 0);
    CHECK(n == 1 && (got.events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0,
          "epoll-read: epoll_wait says the connection is still alive (%d, events %#x)", n, n == 1 ? got.events : 0);
    // (Non-blocking, for the control run's sake: there the connection is
    // alive and a plain read() would wait for ever.)
    fcntl(s[5], F_SETFL, fcntl(s[5], F_GETFL) | O_NONBLOCK);
    errno = 0; res = read(s[5], buf, sizeof(buf)); err = errno;
    reset_expected("epoll-read", "read", res, err, 0);

    if (failures != 0)
        return 1;
    printf("PASS\n");
    return 0;
}
