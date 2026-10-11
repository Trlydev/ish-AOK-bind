// Guest half of sockrestart_listeners.sh: listeners across the app's
// suspend/resume socket cycle (fs/sockrestart.c).
//
// The host script runs this with ISH_SOCKRESTART_AFTER=1, so the save and the
// rebuild happen inside the three-second sleep below, and with each of the
// ISH_SOCKRESTART_TEST_DESTROY modes. One mode per run:
//
//   survive      a unix listener at a path; afterwards a client connects and
//                the accepted connection carries its byte
//   abstract     the same at an abstract name
//   pending      a client connects and writes BEFORE the cycle; afterwards
//                accept must hand back that same connection, and a new client
//                must still get through. Only meaningful when the listener
//                was NOT destroyed: a dead listener's queue dies with it.
//   accept       the server sits in a blocking accept() across the cycle, and
//                a child connects after it
//   poll         the server sits in poll() across the cycle
//   epollet      the server has been told of one connection through an
//                EPOLLIN|EPOLLET epoll, then sits in epoll_wait() across the
//                cycle: the rebuilt listener's host watch has to be put back,
//                because an edge-triggered registration hears of a connection
//                only from the host (fs/poll.c poll_rearm_host_fd)
//   relisten     the guest calls listen() twice on the one socket
//   mixed        a unix and a TCP listener at once, both serving afterwards
//   tcp, tcp-pending, tcp-accept, tcp-epollet
//                survive, pending, accept and epollet over 127.0.0.1
//   epoll-change a thread sleeps in epoll_wait() on two listeners, another
//                thread takes one out of the set, and the sleeper wakes and
//                exits -- all before the cycle. The kernel listed a waiting
//                task per socket at the start of the wait and unlisted it per
//                socket at the end, so the sleeper stayed listed after its
//                exit, and the resume wrote to its freed task (the 557 crash
//                in sockrestart_on_resume). Afterwards the first listener
//                must still serve through the same epoll.
//
// Prints "PASS <mode>" or "FAIL <mode>: ..." and exits 0 or 1.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static const char *mode;
static struct sockaddr_storage addr;
static socklen_t addr_len;

static void fail(const char *what) {
    printf("FAIL %s: %s: %s\n", mode, what, strerror(errno));
    exit(1);
}

// Listen at a fresh name, and leave that name in addr for client().
static int make_listener(int family, int abstract) {
    int s = socket(family, SOCK_STREAM, 0);
    if (s < 0)
        fail("socket");
    memset(&addr, 0, sizeof(addr));
    if (family == AF_UNIX) {
        struct sockaddr_un *un = (struct sockaddr_un *) &addr;
        un->sun_family = AF_UNIX;
        if (abstract) {
            const char *name = "sockrestart-listeners";
            strcpy(un->sun_path + 1, name);
            addr_len = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name);
        } else {
            const char *name = "/tmp/sockrestart-listeners.sock";
            unlink(name);
            strcpy(un->sun_path, name);
            addr_len = sizeof(*un);
        }
    } else {
        struct sockaddr_in *in = (struct sockaddr_in *) &addr;
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr_len = sizeof(*in);
    }
    if (bind(s, (struct sockaddr *) &addr, addr_len) < 0)
        fail("bind");
    if (listen(s, 8) < 0)
        fail("listen");
    if (family == AF_INET) {
        addr_len = sizeof(addr);
        if (getsockname(s, (struct sockaddr *) &addr, &addr_len) < 0)
            fail("getsockname");
    }
    return s;
}

static int client(char byte) {
    int c = socket(addr.ss_family, SOCK_STREAM, 0);
    if (c < 0)
        fail("client socket");
    if (connect(c, (struct sockaddr *) &addr, addr_len) < 0)
        fail("connect");
    if (write(c, &byte, 1) != 1)
        fail("client write");
    return c;
}

// Accept one connection and check that it carries the byte `want`.
static void serve(int s, char want) {
    int a = accept(s, NULL, NULL);
    if (a < 0)
        fail("accept");
    char got = 0;
    if (read(a, &got, 1) != 1)
        fail("read");
    if (got != want) {
        printf("FAIL %s: accepted a connection carrying '%c', wanted '%c'\n", mode, got, want);
        exit(1);
    }
    close(a);
}

static int is(const char *m) {
    return strcmp(mode, m) == 0;
}

static int change_ep;
static volatile int waiter_state;       // 1 going into epoll_wait, 2 out of it
static int waiter_fd = -1;

static void *epoll_waiter(void *arg) {
    (void) arg;
    struct epoll_event ev;
    waiter_state = 1;
    if (epoll_wait(change_ep, &ev, 1, -1) == 1)
        waiter_fd = ev.data.fd;
    waiter_state = 2;
    return NULL;
}

static void *idle_thread(void *arg) {
    return arg;
}

// The kernel says so when it frees a task still listed as waiting on a
// socket. A root without a readable /dev/kmsg skips the check.
static void check_kernel_log(void) {
    int k = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (k < 0)
        return;
    char rec[2048];
    ssize_t n;
    while ((n = read(k, rec, sizeof(rec) - 1)) > 0) {
        rec[n] = '\0';
        if (strstr(rec, "freed still listed") != NULL) {
            printf("FAIL %s: the kernel log has: %s", mode, rec);
            exit(1);
        }
    }
    close(k);
}

int main(int argc, char **argv) {
    mode = argc > 1 ? argv[1] : "survive";
    alarm(20);
    setvbuf(stdout, NULL, _IONBF, 0);
    int s = make_listener(strncmp(mode, "tcp", 3) == 0 ? AF_INET : AF_UNIX, is("abstract"));

    if (is("pending") || is("tcp-pending")) {
        int early = client('p');
        sleep(3);
        serve(s, 'p');
        close(early);
        int late = client('n');
        serve(s, 'n');
        close(late);
    } else if (is("mixed")) {
        struct sockaddr_storage unix_addr = addr;
        socklen_t unix_len = addr_len;
        int t = make_listener(AF_INET, 0);
        sleep(3);
        int c = client('t');
        serve(t, 't');
        close(c);
        addr = unix_addr;
        addr_len = unix_len;
        c = client('u');
        serve(s, 'u');
        close(c);
    } else if (is("accept") || is("tcp-accept") || is("poll")) {
        pid_t pid = fork();
        if (pid < 0)
            fail("fork");
        if (pid == 0) {
            sleep(3);
            int c = client('c');
            sleep(1);
            close(c);
            _exit(0);
        }
        if (is("poll")) {
            struct pollfd p = {.fd = s, .events = POLLIN};
            if (poll(&p, 1, 15000) != 1)
                fail("poll");
        }
        serve(s, 'c');
        int status;
        waitpid(pid, &status, 0);
    } else if (is("epollet") || is("tcp-epollet")) {
        int ep = epoll_create1(0);
        struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.fd = s};
        if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev) < 0)
            fail("epoll");
        int early = client('e');
        if (epoll_wait(ep, &ev, 1, 5000) != 1)
            fail("epoll_wait before the cycle");
        serve(s, 'e');
        close(early);
        pid_t pid = fork();
        if (pid < 0)
            fail("fork");
        if (pid == 0) {
            sleep(3);
            int c = client('c');
            sleep(1);
            close(c);
            _exit(0);
        }
        errno = 0;
        if (epoll_wait(ep, &ev, 1, 15000) != 1)
            fail("epoll_wait across the cycle");
        serve(s, 'c');
        int status;
        waitpid(pid, &status, 0);
    } else if (is("epoll-change")) {
        struct sockaddr_storage first_addr = addr;
        socklen_t first_len = addr_len;
        int other = make_listener(AF_UNIX, 1);
        addr = first_addr;
        addr_len = first_len;
        change_ep = epoll_create1(0);
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = s};
        if (change_ep < 0 || epoll_ctl(change_ep, EPOLL_CTL_ADD, s, &ev) < 0)
            fail("epoll");
        ev.data.fd = other;
        if (epoll_ctl(change_ep, EPOLL_CTL_ADD, other, &ev) < 0)
            fail("epoll, second listener");
        pthread_t waiter;
        if (pthread_create(&waiter, NULL, epoll_waiter, NULL) != 0)
            fail("pthread_create");
        while (waiter_state == 0)
            usleep(1000);
        usleep(200000);
        // The set has to change while the waiter is inside its wait, or this
        // run shows nothing: it is still inside before the removal and after.
        if (waiter_state != 1) {
            printf("FAIL %s: the waiter left epoll_wait before the set changed\n", mode);
            return 1;
        }
        if (epoll_ctl(change_ep, EPOLL_CTL_DEL, other, NULL) < 0)
            fail("EPOLL_CTL_DEL");
        if (waiter_state != 1) {
            printf("FAIL %s: the waiter left epoll_wait during the removal\n", mode);
            return 1;
        }
        int early = client('e');
        pthread_join(waiter, NULL);
        if (waiter_fd != s) {
            printf("FAIL %s: the waiter woke for fd %d, wanted %d\n", mode, waiter_fd, s);
            return 1;
        }
        serve(s, 'e');
        close(early);
        // The waiter's task is gone. Give its memory to other tasks, so that
        // a kernel still holding it writes to something live.
        for (int i = 0; i < 64; i++) {
            pthread_t t;
            if (pthread_create(&t, NULL, idle_thread, NULL) != 0)
                fail("pthread_create, churn");
            pthread_join(t, NULL);
        }
        sleep(3);
        int late = client('n');
        if (epoll_wait(change_ep, &ev, 1, 5000) != 1 || ev.data.fd != s)
            fail("epoll_wait after the cycle");
        serve(s, 'n');
        close(late);
        check_kernel_log();
    } else if (is("survive") || is("abstract") || is("relisten") || is("tcp")) {
        if (is("relisten") && listen(s, 16) < 0)
            fail("second listen");
        sleep(3);
        int c = client('s');
        serve(s, 's');
        close(c);
    } else {
        printf("FAIL %s: no such mode\n", mode);
        return 1;
    }
    printf("PASS %s\n", mode);
    return 0;
}
