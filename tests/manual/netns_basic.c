// Network namespaces: a network of a process's own -- which, for a sandbox,
// means none.
//
// AOK had one network: clone(CLONE_NEWNET) was EINVAL and unshare ENOSYS.
// bubblewrap --unshare-net (the last of what Codex CLI's command sandbox asks
// for) makes one and brings its loopback up over rtnetlink. kernel/net_ns.h
// has the model: a private namespace has "lo" and nothing else, and its
// sockets never touch the host's network.
//
// As an ordinary user in a user namespace, which is how it runs on Linux as
// the oracle (6.12, camd); as root the test becomes uid 1000 first. What is
// checked on both:
//
//   new          /proc/self/ns/net names a new namespace; "lo" is index 1 and
//                down; a link dump over rtnetlink lists it and nothing else;
//                before it is up there is no route even to 127.0.0.1
//   lo up        RTM_NEWADDR 127.0.0.1/8 and RTM_NEWLINK IFF_UP, as
//                bubblewrap sends them, are both acknowledged; "lo" then
//                reads UP and an address dump has 127.0.0.1
//   unreachable  a connect or a send to an address outside is ENETUNREACH; a
//                TCP connect to a loopback port nobody listens on is
//                ECONNREFUSED
//   bind         127.0.0.1 with port 0 is given a port, reported back by
//                getsockname; an address that is not the namespace's is
//                EADDRNOTAVAIL
//   abstract     an abstract AF_UNIX name bound outside is not found from
//                inside (ECONNREFUSED), and the same name can be bound
//                inside; a pathname socket is reached, the filesystem being
//                shared
//   old sockets  a socket made before the unshare is still in the network it
//                was made in: a datagram sent through it arrives outside,
//                and one sent through a socket made after does not
//
//   loopback     inside the namespace its own traffic flows: a TCP listener
//                on 127.0.0.1 is connected to, accept() names the client by
//                the address the client's getsockname gives, data goes both
//                ways, and getpeername still answers after the peer has
//                closed; a second bind of the port is EADDRINUSE; a datagram
//                reaches a bound socket with its sender's address, and the
//                reply to that address comes back; one to a port nobody has
//                is sent and gone; the same over ::1. None of it is the
//                machine's: the port the listener has inside is refused from
//                outside, and a listener on the machine's loopback is refused
//                from inside
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define NAME "netns_basic"
static int failures;
static uid_t outer_uid;
static gid_t outer_gid;

#define MUST(cond, ...) do { if (!(cond)) { printf(NAME ": " __VA_ARGS__); printf(" (errno %d: %s)\n", errno, strerror(errno)); fflush(stdout); _exit(1 + __LINE__ % 200); } } while (0)

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return errno;
    ssize_t n = write(fd, text, strlen(text));
    int err = n < 0 ? errno : 0;
    close(fd);
    return err;
}

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

static struct sockaddr_in in_addr(const char *ip, int port) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t) port)};
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

// 0, or the errno.
static int connect_errno(int type, const char *ip, int port) {
    int s = socket(AF_INET, type, 0);
    if (s < 0)
        return errno;
    struct sockaddr_in a = in_addr(ip, port);
    int err = connect(s, (struct sockaddr *) &a, sizeof(a)) == 0 ? 0 : errno;
    close(s);
    return err;
}

static int lo_flags(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr = {0};
    strcpy(ifr.ifr_name, "lo");
    int flags = ioctl(s, SIOCGIFFLAGS, &ifr) == 0 ? ifr.ifr_flags : -1;
    close(s);
    return flags;
}

// One rtnetlink request and its acknowledgement: 0, or the errno it carries.
static int rtnl_ack(int nl, void *req, size_t len) {
    struct nlmsghdr *hdr = req;
    static unsigned seq = 100;
    hdr->nlmsg_seq = ++seq;
    hdr->nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
    if (send(nl, req, len, 0) != (ssize_t) len)
        return errno;
    char buf[4096];
    ssize_t n = recv(nl, buf, sizeof(buf), 0);
    if (n < (ssize_t) sizeof(struct nlmsghdr))
        return EIO;
    struct nlmsghdr *reply = (struct nlmsghdr *) buf;
    if (reply->nlmsg_type != NLMSG_ERROR || reply->nlmsg_seq != seq)
        return EPROTO;
    return -((struct nlmsgerr *) NLMSG_DATA(reply))->error;
}

// What bubblewrap's loopback_setup sends.
static void bring_lo_up(void) {
    int nl = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    MUST(nl >= 0, "no rtnetlink socket");
    struct sockaddr_nl self = {.nl_family = AF_NETLINK, .nl_pid = (unsigned) getpid()};
    MUST(bind(nl, (struct sockaddr *) &self, sizeof(self)) == 0, "binding the rtnetlink socket failed");
    struct {
        struct nlmsghdr hdr;
        struct ifaddrmsg addr;
        char attrs[64];
    } newaddr = {.hdr.nlmsg_type = RTM_NEWADDR, .hdr.nlmsg_flags = NLM_F_CREATE | NLM_F_EXCL,
                 .addr = {.ifa_family = AF_INET, .ifa_prefixlen = 8, .ifa_flags = IFA_F_PERMANENT,
                          .ifa_scope = RT_SCOPE_HOST, .ifa_index = 1}};
    struct in_addr lo = {.s_addr = htonl(INADDR_LOOPBACK)};
    size_t len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    for (int type = IFA_LOCAL; type >= IFA_ADDRESS; type--) {
        struct rtattr *attr = (struct rtattr *) ((char *) &newaddr + NLMSG_ALIGN(len));
        attr->rta_type = (unsigned short) type;
        attr->rta_len = RTA_LENGTH(sizeof(lo));
        memcpy(RTA_DATA(attr), &lo, sizeof(lo));
        len = NLMSG_ALIGN(len) + RTA_ALIGN(attr->rta_len);
    }
    newaddr.hdr.nlmsg_len = (unsigned) len;
    int e = rtnl_ack(nl, &newaddr, len);
    MUST(e == 0, "RTM_NEWADDR 127.0.0.1/8 on lo was answered with errno %d", e);
    struct {
        struct nlmsghdr hdr;
        struct ifinfomsg info;
    } newlink = {.hdr.nlmsg_type = RTM_NEWLINK, .hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)),
                 .info = {.ifi_family = AF_UNSPEC, .ifi_index = 1, .ifi_flags = IFF_UP, .ifi_change = IFF_UP}};
    e = rtnl_ack(nl, &newlink, newlink.hdr.nlmsg_len);
    MUST(e == 0, "RTM_NEWLINK IFF_UP on lo was answered with errno %d", e);
    close(nl);
}

// How many links a dump lists, and whether "lo" with index 1 is among them.
static int link_dump(bool *has_lo) {
    int nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    struct {
        struct nlmsghdr hdr;
        struct ifinfomsg info;
    } req = {.hdr = {.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg)), .nlmsg_type = RTM_GETLINK,
                     .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP, .nlmsg_seq = 7},
             .info.ifi_family = AF_UNSPEC};
    *has_lo = false;
    if (nl < 0 || send(nl, &req, req.hdr.nlmsg_len, 0) < 0)
        return -1;
    int links = 0;
    for (;;) {
        char buf[8192];
        ssize_t n = recv(nl, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        for (struct nlmsghdr *h = (struct nlmsghdr *) buf; NLMSG_OK(h, (unsigned) n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR) {
                close(nl);
                return links;
            }
            if (h->nlmsg_type != RTM_NEWLINK)
                continue;
            links++;
            struct ifinfomsg *info = NLMSG_DATA(h);
            int attr_len = (int) (h->nlmsg_len - NLMSG_LENGTH(sizeof(*info)));
            for (struct rtattr *a = IFLA_RTA(info); RTA_OK(a, attr_len); a = RTA_NEXT(a, attr_len))
                if (a->rta_type == IFLA_IFNAME && strcmp(RTA_DATA(a), "lo") == 0 && info->ifi_index == 1)
                    *has_lo = true;
        }
    }
    close(nl);
    return links;
}

static void case_new_and_up(void) {
    char before[64], after[64];
    ssize_t n = readlink("/proc/self/ns/net", before, sizeof(before) - 1);
    MUST(n > 0, "no /proc/self/ns/net");
    before[n] = '\0';
    enter(CLONE_NEWNET);
    n = readlink("/proc/self/ns/net", after, sizeof(after) - 1);
    MUST(n > 0, "no /proc/self/ns/net after the unshare");
    after[n] = '\0';
    MUST(strcmp(before, after) != 0, "ns/net is still %s after the unshare", before);

    MUST(if_nametoindex("lo") == 1, "\"lo\" is index %u in a new network namespace", if_nametoindex("lo"));
    int flags = lo_flags();
    MUST(flags >= 0 && (flags & IFF_LOOPBACK) && !(flags & IFF_UP), "a new namespace's lo has flags %#x, wanted LOOPBACK and not UP", flags);
    bool has_lo;
    int links = link_dump(&has_lo);
    MUST(links == 1 && has_lo, "a link dump lists %d links (lo among them: %d); wanted lo alone", links, has_lo);
    int e = connect_errno(SOCK_STREAM, "127.0.0.1", 9);
    MUST(e == ENETUNREACH, "connect to 127.0.0.1 with lo down: errno %d, wanted ENETUNREACH", e);

    bring_lo_up();
    flags = lo_flags();
    MUST(flags >= 0 && (flags & IFF_UP), "after RTM_NEWLINK IFF_UP, lo has flags %#x", flags);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr = {0};
    strcpy(ifr.ifr_name, "lo");
    MUST(ioctl(s, SIOCGIFADDR, &ifr) == 0 &&
         ((struct sockaddr_in *) &ifr.ifr_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK),
         "lo's address is not 127.0.0.1 after RTM_NEWADDR");
    close(s);

    // Nothing outside, and nobody home on loopback.
    e = connect_errno(SOCK_STREAM, "192.0.2.1", 80);
    MUST(e == ENETUNREACH, "TCP connect to an address outside: errno %d, wanted ENETUNREACH", e);
    e = connect_errno(SOCK_DGRAM, "192.0.2.1", 53);
    MUST(e == ENETUNREACH, "UDP connect to an address outside: errno %d, wanted ENETUNREACH", e);
    s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in out = in_addr("192.0.2.1", 53);
    errno = 0;
    MUST(sendto(s, "x", 1, 0, (struct sockaddr *) &out, sizeof(out)) == -1 && errno == ENETUNREACH,
         "a datagram to an address outside did not fail with ENETUNREACH");
    close(s);
    e = connect_errno(SOCK_STREAM, "127.0.0.1", 9);
    MUST(e == ECONNREFUSED, "TCP connect to a loopback port nobody listens on: errno %d, wanted ECONNREFUSED", e);

    // bind
    s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in any_port = in_addr("127.0.0.1", 0), got = {0};
    socklen_t got_len = sizeof(got);
    MUST(bind(s, (struct sockaddr *) &any_port, sizeof(any_port)) == 0, "bind to 127.0.0.1:0 failed");
    MUST(getsockname(s, (struct sockaddr *) &got, &got_len) == 0 && got.sin_family == AF_INET &&
         got.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && got.sin_port != 0,
         "getsockname after bind to 127.0.0.1:0 says port %d", ntohs(got.sin_port));
    close(s);
    s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in foreign = in_addr("192.0.2.5", 0);
    errno = 0;
    MUST(bind(s, (struct sockaddr *) &foreign, sizeof(foreign)) == -1 && errno == EADDRNOTAVAIL,
         "bind to an address the namespace does not have did not fail with EADDRNOTAVAIL");
    close(s);
}

static int machine_tcp_port;    // a listener on the machine's loopback, made by the parent

static void case_loopback(void) {
    enter(CLONE_NEWNET);
    bring_lo_up();

    // TCP.
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in any_port = in_addr("127.0.0.1", 0), laddr = {0}, caddr = {0}, peer = {0}, got = {0};
    socklen_t len = sizeof(laddr);
    MUST(bind(l, (struct sockaddr *) &any_port, sizeof(any_port)) == 0 && listen(l, 4) == 0, "bind and listen on 127.0.0.1 inside the namespace failed");
    MUST(getsockname(l, (struct sockaddr *) &laddr, &len) == 0 && laddr.sin_port != 0, "the listener has no port");
    int dup_bind = socket(AF_INET, SOCK_STREAM, 0);
    errno = 0;
    MUST(bind(dup_bind, (struct sockaddr *) &laddr, sizeof(laddr)) == -1 && errno == EADDRINUSE, "a second bind of the listener's port did not fail with EADDRINUSE");
    close(dup_bind);
    int c = socket(AF_INET, SOCK_STREAM, 0);
    MUST(connect(c, (struct sockaddr *) &laddr, sizeof(laddr)) == 0, "connect to the listener inside the namespace failed");
    len = sizeof(caddr);
    MUST(getsockname(c, (struct sockaddr *) &caddr, &len) == 0 && caddr.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && caddr.sin_port != 0,
         "the connected socket's own address is %s:%d", inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));
    len = sizeof(peer);
    int a = accept(l, (struct sockaddr *) &peer, &len);
    MUST(a >= 0, "accept failed");
    MUST(len == sizeof(peer) && peer.sin_family == AF_INET && peer.sin_addr.s_addr == caddr.sin_addr.s_addr && peer.sin_port == caddr.sin_port,
         "accept names its peer %s:%d; the client is port %d", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port), ntohs(caddr.sin_port));
    char buf[16] = "";
    MUST(write(c, "ping", 4) == 4 && read(a, buf, sizeof(buf)) == 4 && memcmp(buf, "ping", 4) == 0, "data did not cross from client to server");
    MUST(write(a, "pong!", 5) == 5 && read(c, buf, sizeof(buf)) == 5 && memcmp(buf, "pong!", 5) == 0, "data did not cross from server to client");
    int one = 1;
    MUST(setsockopt(c, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof(one)) == 0, "TCP_NODELAY was refused");
    close(a);
    MUST(read(c, buf, sizeof(buf)) == 0, "the client did not read end-of-file after the server closed");
    len = sizeof(got);
    MUST(getpeername(c, (struct sockaddr *) &got, &len) == 0 && got.sin_port == laddr.sin_port, "getpeername after the peer closed: port %d, wanted %d", ntohs(got.sin_port), ntohs(laddr.sin_port));
    close(c);
    close(l);

    // UDP.
    int r = socket(AF_INET, SOCK_DGRAM, 0), u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in raddr = {0}, from = {0}, uaddr = {0};
    MUST(bind(r, (struct sockaddr *) &any_port, sizeof(any_port)) == 0, "UDP bind failed");
    len = sizeof(raddr);
    MUST(getsockname(r, (struct sockaddr *) &raddr, &len) == 0 && raddr.sin_port != 0, "the UDP socket has no port");
    MUST(sendto(u, "dgram", 5, 0, (struct sockaddr *) &raddr, sizeof(raddr)) == 5, "sendto inside the namespace failed");
    len = sizeof(from);
    MUST(recvfrom(r, buf, sizeof(buf), 0, (struct sockaddr *) &from, &len) == 5 && memcmp(buf, "dgram", 5) == 0, "the datagram did not arrive");
    len = sizeof(uaddr);
    MUST(getsockname(u, (struct sockaddr *) &uaddr, &len) == 0 && uaddr.sin_port != 0, "the sender was given no port");
    MUST(from.sin_family == AF_INET && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && from.sin_port == uaddr.sin_port,
         "the datagram's sender reads %s:%d; it is port %d", inet_ntoa(from.sin_addr), ntohs(from.sin_port), ntohs(uaddr.sin_port));
    MUST(sendto(r, "reply", 5, 0, (struct sockaddr *) &from, sizeof(from)) == 5 && recv(u, buf, sizeof(buf), 0) == 5 && memcmp(buf, "reply", 5) == 0,
         "the reply to the sender's address did not come back");
    struct sockaddr_in nobody = in_addr("127.0.0.1", 9);
    MUST(sendto(u, "x", 1, 0, (struct sockaddr *) &nobody, sizeof(nobody)) == 1, "a datagram to a port nobody has was not accepted for sending");
    close(u);
    close(r);

    // The same over ::1.
    int l6 = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 a6 = {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT}, got6 = {0};
    MUST(bind(l6, (struct sockaddr *) &a6, sizeof(a6)) == 0 && listen(l6, 1) == 0, "bind and listen on ::1 failed");
    len = sizeof(got6);
    MUST(getsockname(l6, (struct sockaddr *) &got6, &len) == 0 && got6.sin6_port != 0, "the ::1 listener has no port");
    int c6 = socket(AF_INET6, SOCK_STREAM, 0);
    MUST(connect(c6, (struct sockaddr *) &got6, sizeof(got6)) == 0, "connect over ::1 failed");
    int a6fd = accept(l6, NULL, NULL);
    MUST(a6fd >= 0 && write(c6, "six", 3) == 3 && read(a6fd, buf, sizeof(buf)) == 3, "data did not cross over ::1");
    close(a6fd);
    close(c6);
    close(l6);

    // And none of it is the machine's: its listener is not here.
    int e = connect_errno(SOCK_STREAM, "127.0.0.1", machine_tcp_port);
    MUST(e == ECONNREFUSED, "the machine's loopback listener answered from inside the namespace (errno %d)", e);

    // A listener left for the parent to find absent on the machine's side.
    int stay = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in fixed = in_addr("127.0.0.1", machine_tcp_port + 1);
    if (bind(stay, (struct sockaddr *) &fixed, sizeof(fixed)) == 0 && listen(stay, 1) == 0) {
        MUST(write(3, "l", 1) == 1, "cannot tell the parent");
        char c2;
        (void) !read(4, &c2, 1);
    } else {
        MUST(write(3, "n", 1) == 1, "cannot tell the parent");
    }
}

static struct sockaddr_un abstract_name(socklen_t *len) {
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    int n = snprintf(a.sun_path + 1, sizeof(a.sun_path) - 1, "netns_basic.%d", (int) getppid());
    *len = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + (size_t) n);
    return a;
}

static char unix_path[64];

static void case_unix(void) {
    // The parent (outside) holds an abstract listener and a pathname one.
    socklen_t len;
    struct sockaddr_un name = abstract_name(&len);
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    MUST(connect(s, (struct sockaddr *) &name, len) == 0, "the control: the abstract name is not reachable before the unshare");
    close(s);

    enter(CLONE_NEWNET);
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    errno = 0;
    MUST(connect(s, (struct sockaddr *) &name, len) == -1 && errno == ECONNREFUSED,
         "an abstract name bound outside the namespace was found from inside");
    close(s);
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    MUST(bind(s, (struct sockaddr *) &name, len) == 0, "the same abstract name could not be bound inside the namespace");
    close(s);
    struct sockaddr_un path = {.sun_family = AF_UNIX};
    strcpy(path.sun_path, unix_path);
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    MUST(connect(s, (struct sockaddr *) &path, sizeof(path)) == 0, "a pathname socket outside was not reached from inside");
    close(s);
}

static int udp_port;    // the parent's datagram socket, on the machine's loopback

static void case_old_sockets(void) {
    int old = socket(AF_INET, SOCK_DGRAM, 0);
    MUST(old >= 0, "socket");
    enter(CLONE_NEWNET);
    bring_lo_up();
    struct sockaddr_in to = in_addr("127.0.0.1", udp_port);
    int fresh = socket(AF_INET, SOCK_DGRAM, 0);
    // Through the new one first: it must not arrive (whether the send says
    // so is the kernel's own business).
    (void) !sendto(fresh, "N", 1, 0, (struct sockaddr *) &to, sizeof(to));
    MUST(sendto(old, "O", 1, 0, (struct sockaddr *) &to, sizeof(to)) == 1,
         "a socket made before the unshare can no longer send where it could");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (geteuid() == 0) {
        if (setgroups(0, NULL) != 0 || setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
            printf(NAME ": FAIL setup: cannot become uid 1000: %s\n", strerror(errno));
            return 1;
        }
    }
    outer_uid = geteuid();
    outer_gid = getegid();
    fflush(stdout);
    pid_t probe = fork();
    if (probe == 0)
        _exit(unshare(CLONE_NEWUSER | CLONE_NEWNET) == 0 ? 0 : 8);
    int status = 0;
    waitpid(probe, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 8) {
        printf(NAME ": SKIP (an ordinary user cannot make a network namespace here)\n");
        return 0;
    }

    run("a new namespace, and its loopback", case_new_and_up);

    // The listeners the unix case looks for from inside.
    struct sockaddr_un name = {.sun_family = AF_UNIX};
    int n = snprintf(name.sun_path + 1, sizeof(name.sun_path) - 1, "netns_basic.%d", (int) getpid());
    int abstract = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(abstract, (struct sockaddr *) &name, (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + (size_t) n)) != 0 ||
            listen(abstract, 4) != 0) {
        printf(NAME ": FAIL setup: abstract listener: %s\n", strerror(errno));
        return 1;
    }
    snprintf(unix_path, sizeof(unix_path), "/tmp/netns_basic.%d.sock", (int) getpid());
    unlink(unix_path);
    struct sockaddr_un path = {.sun_family = AF_UNIX};
    strcpy(path.sun_path, unix_path);
    int named = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(named, (struct sockaddr *) &path, sizeof(path)) != 0 || listen(named, 4) != 0 || chmod(unix_path, 0666) != 0) {
        printf(NAME ": FAIL setup: pathname listener: %s\n", strerror(errno));
        return 1;
    }
    run("unix sockets", case_unix);
    close(abstract);
    close(named);
    unlink(unix_path);

    // Loopback inside a namespace, and the machine's loopback outside it.
    {
        int machine = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in any = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}, got = {0};
        socklen_t got_len = sizeof(got);
        if (bind(machine, (struct sockaddr *) &any, sizeof(any)) != 0 || listen(machine, 4) != 0 ||
                getsockname(machine, (struct sockaddr *) &got, &got_len) != 0) {
            printf(NAME ": FAIL setup: the machine's listener: %s\n", strerror(errno));
            return 1;
        }
        machine_tcp_port = ntohs(got.sin_port);
        int up[2], down[2];
        if (pipe(up) != 0 || pipe(down) != 0)
            return 1;
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            dup2(up[1], 3);
            dup2(down[0], 4);
            close(up[0]);
            close(down[1]);
            case_loopback();
            _exit(0);
        }
        close(up[1]);
        close(down[0]);
        char told = 0;
        if (read(up[0], &told, 1) == 1 && told == 'l') {
            // The child is listening on machine_tcp_port + 1 INSIDE its
            // namespace. Out here that port has nobody.
            int e = connect_errno(SOCK_STREAM, "127.0.0.1", machine_tcp_port + 1);
            if (e != ECONNREFUSED) {
                printf(NAME ": a listener inside a namespace answered on the machine's loopback (errno %d)\n", e);
                failures++;
            }
        }
        close(down[1]);
        int status = -1;
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf(NAME ": loopback: child status %#x\n", status);
            failures++;
        }
        close(up[0]);
        close(machine);
    }

    // A datagram socket on the machine's loopback, for the old-socket case.
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in any = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}, got = {0};
    socklen_t got_len = sizeof(got);
    if (bind(udp, (struct sockaddr *) &any, sizeof(any)) != 0 || getsockname(udp, (struct sockaddr *) &got, &got_len) != 0) {
        printf(NAME ": FAIL setup: datagram socket: %s\n", strerror(errno));
        return 1;
    }
    udp_port = ntohs(got.sin_port);
    run("sockets made before the unshare", case_old_sockets);
    // Exactly one datagram: the one from the socket that was made outside.
    char received[8] = "";
    int count = 0;
    for (int i = 0; i < 20; i++) {
        char c;
        if (recv(udp, &c, 1, MSG_DONTWAIT) == 1 && count < 7)
            received[count++] = c;
        else if (count > 0 && i > 5)
            break;
        else
            usleep(50000);
    }
    if (strcmp(received, "O") != 0) {
        printf(NAME ": the machine's socket received \"%s\"; wanted \"O\", the datagram sent through the socket made before the unshare, alone\n", received);
        failures++;
    }
    close(udp);

    if (failures != 0) {
        printf(NAME ": FAIL failures=%d\n", failures);
        return 1;
    }
    printf(NAME ": PASS\n");
    return 0;
}
