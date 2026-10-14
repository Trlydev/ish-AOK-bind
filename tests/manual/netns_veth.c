// A second interface in a network namespace: veth pairs and dummy links.
//
// A private network namespace here was "lo" and nothing else: RTM_NEWLINK of
// any kind was EOPNOTSUPP, so a sandbox could not be wired to another, and
// nothing that sets one up (ip link add ... type veth, systemd-nspawn
// --network-veth, a CNI plugin) got past its first step. kernel/net_ns.h has
// the model: a virtual interface is a name, an index and addresses in one
// namespace, and a veth's two ends carry the namespaces' own traffic --
// TCP, UDP and ICMP echo, between the emulated sockets of each side.
//
// Run as an ordinary user in a user namespace, which is how it runs against
// Linux 6.12 (camd); as root the test becomes uid 1000 first. "Here" is the
// test's own network namespace, "there" a child's, both in one user
// namespace.
//
//   create       RTM_NEWLINK kind "veth" with the peer named and put in the
//                child's namespace by pid: the near end is here and the far
//                end there, each in its namespace's dumps, and only there
//   addresses    10.200.0.1/24 here, 10.200.0.2/24 there; a dump of either
//                namespace's addresses and routes has its own
//   tcp          a listener there on the wildcard is reached at
//                10.200.0.2, and names the client 10.200.0.1, which is also
//                what the client's getsockname says; data both ways
//   udp          a datagram to 10.200.0.2 arrives from 10.200.0.1, and the
//                reply comes back from 10.200.0.2
//   scoped       a listener there bound to 127.0.0.1 is not reached at
//                10.200.0.2 (ECONNREFUSED); an address with no route is
//                ENETUNREACH
//   ping         a raw ICMP echo to 10.200.0.2 is answered from it
//   move         a link moved to the child's namespace by pid leaves this one
//   delete       deleting one end of a pair takes the other
//   dummy        a dummy link with 10.9.9.9/32 is reached from its own
//                namespace at that address
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define NAME "netns_veth"
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf(NAME ": " __VA_ARGS__); printf(" (errno %d: %s)\n", errno, strerror(errno)); failures++; } } while (0)
#define MUST(cond, ...) do { if (!(cond)) { printf(NAME ": " __VA_ARGS__); printf(" (errno %d: %s)\n", errno, strerror(errno)); fflush(stdout); _exit(1 + __LINE__ % 200); } } while (0)

#ifndef VETH_INFO_PEER
#define VETH_INFO_PEER 1
#endif

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return errno;
    ssize_t n = write(fd, text, strlen(text));
    int err = n < 0 ? errno : 0;
    close(fd);
    return err;
}

// ---- a little rtnetlink ----------------------------------------------------

struct req {
    char buf[1024];
    struct nlmsghdr *hdr;
};

static void req_init(struct req *r, int type, int flags, size_t body) {
    memset(r->buf, 0, sizeof(r->buf));
    r->hdr = (struct nlmsghdr *) r->buf;
    r->hdr->nlmsg_len = NLMSG_LENGTH(body);
    r->hdr->nlmsg_type = (uint16_t) type;
    r->hdr->nlmsg_flags = (uint16_t) (NLM_F_REQUEST | flags);
    r->hdr->nlmsg_seq = 1;
}

static void *req_body(struct req *r) { return NLMSG_DATA(r->hdr); }

static struct rtattr *attr_add(struct req *r, int type, const void *data, size_t len) {
    struct rtattr *a = (struct rtattr *) (r->buf + NLMSG_ALIGN(r->hdr->nlmsg_len));
    a->rta_type = (uint16_t) type;
    a->rta_len = (uint16_t) RTA_LENGTH(len);
    if (len != 0)
        memcpy(RTA_DATA(a), data, len);
    r->hdr->nlmsg_len = NLMSG_ALIGN(r->hdr->nlmsg_len) + RTA_ALIGN(a->rta_len);
    return a;
}

static void attr_end(struct req *r, struct rtattr *nest) {
    nest->rta_len = (uint16_t) (r->buf + r->hdr->nlmsg_len - (char *) nest);
}

// The request's ack: 0 or -errno.
static int nl_call(struct req *r) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -errno;
    r->hdr->nlmsg_flags |= NLM_F_ACK;
    if (send(fd, r->buf, r->hdr->nlmsg_len, 0) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    char reply[4096];
    ssize_t n = recv(fd, reply, sizeof(reply), 0);
    close(fd);
    if (n < (ssize_t) sizeof(struct nlmsghdr))
        return -EIO;
    struct nlmsghdr *h = (struct nlmsghdr *) reply;
    if (h->nlmsg_type != NLMSG_ERROR)
        return -EIO;
    return ((struct nlmsgerr *) NLMSG_DATA(h))->error;
}

static int veth_add(const char *name, const char *peer, pid_t peer_ns_pid) {
    struct req r;
    req_init(&r, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL, sizeof(struct ifinfomsg));
    attr_add(&r, IFLA_IFNAME, name, strlen(name) + 1);
    struct rtattr *info = attr_add(&r, IFLA_LINKINFO, NULL, 0);
    attr_add(&r, IFLA_INFO_KIND, "veth", 5);
    struct rtattr *data = attr_add(&r, IFLA_INFO_DATA, NULL, 0);
    struct rtattr *peer_attr = attr_add(&r, VETH_INFO_PEER, NULL, 0);
    // The peer's own ifinfomsg, then its attributes.
    struct ifinfomsg empty = {0};
    memcpy(r.buf + r.hdr->nlmsg_len, &empty, sizeof(empty));
    r.hdr->nlmsg_len += NLMSG_ALIGN(sizeof(empty));
    attr_add(&r, IFLA_IFNAME, peer, strlen(peer) + 1);
    if (peer_ns_pid > 0) {
        uint32_t pid = (uint32_t) peer_ns_pid;
        attr_add(&r, IFLA_NET_NS_PID, &pid, sizeof(pid));
    }
    attr_end(&r, peer_attr);
    attr_end(&r, data);
    attr_end(&r, info);
    return nl_call(&r);
}

static int link_add_kind(const char *name, const char *kind) {
    struct req r;
    req_init(&r, RTM_NEWLINK, NLM_F_CREATE | NLM_F_EXCL, sizeof(struct ifinfomsg));
    attr_add(&r, IFLA_IFNAME, name, strlen(name) + 1);
    struct rtattr *info = attr_add(&r, IFLA_LINKINFO, NULL, 0);
    attr_add(&r, IFLA_INFO_KIND, kind, strlen(kind) + 1);
    attr_end(&r, info);
    return nl_call(&r);
}

static int link_up(const char *name) {
    struct req r;
    req_init(&r, RTM_NEWLINK, 0, sizeof(struct ifinfomsg));
    struct ifinfomsg *msg = req_body(&r);
    msg->ifi_index = (int) if_nametoindex(name);
    msg->ifi_flags = IFF_UP;
    msg->ifi_change = IFF_UP;
    return msg->ifi_index == 0 ? -ENODEV : nl_call(&r);
}

static int link_to_ns(const char *name, pid_t pid) {
    struct req r;
    req_init(&r, RTM_NEWLINK, 0, sizeof(struct ifinfomsg));
    struct ifinfomsg *msg = req_body(&r);
    msg->ifi_index = (int) if_nametoindex(name);
    uint32_t p = (uint32_t) pid;
    attr_add(&r, IFLA_NET_NS_PID, &p, sizeof(p));
    return msg->ifi_index == 0 ? -ENODEV : nl_call(&r);
}

static int link_del(const char *name) {
    struct req r;
    req_init(&r, RTM_DELLINK, 0, sizeof(struct ifinfomsg));
    struct ifinfomsg *msg = req_body(&r);
    msg->ifi_index = (int) if_nametoindex(name);
    return msg->ifi_index == 0 ? -ENODEV : nl_call(&r);
}

static int addr_add(const char *name, const char *ip, int prefix) {
    struct req r;
    req_init(&r, RTM_NEWADDR, NLM_F_CREATE | NLM_F_EXCL, sizeof(struct ifaddrmsg));
    struct ifaddrmsg *msg = req_body(&r);
    msg->ifa_family = AF_INET;
    msg->ifa_prefixlen = (uint8_t) prefix;
    msg->ifa_index = if_nametoindex(name);
    struct in_addr a;
    inet_pton(AF_INET, ip, &a);
    attr_add(&r, IFA_LOCAL, &a, sizeof(a));
    attr_add(&r, IFA_ADDRESS, &a, sizeof(a));
    return msg->ifa_index == 0 ? -ENODEV : nl_call(&r);
}

// What a dump of `type` says, as one string of "name;" for links, of
// "a.b.c.d/p;" for addresses, and "a.b.c.d/p@ifindex;" for routes.
static int dump(int type, char *out, size_t size) {
    out[0] = '\0';
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -errno;
    struct req r;
    req_init(&r, type, NLM_F_DUMP, type == RTM_GETLINK ? sizeof(struct ifinfomsg) :
             type == RTM_GETADDR ? sizeof(struct ifaddrmsg) : sizeof(struct rtmsg));
    if (type == RTM_GETADDR)
        ((struct ifaddrmsg *) req_body(&r))->ifa_family = AF_INET;
    if (type == RTM_GETROUTE)
        ((struct rtmsg *) req_body(&r))->rtm_family = AF_INET;
    if (send(fd, r.buf, r.hdr->nlmsg_len, 0) < 0) {
        close(fd);
        return -errno;
    }
    char buf[16384];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        for (struct nlmsghdr *h = (struct nlmsghdr *) buf; NLMSG_OK(h, (unsigned) n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR) {
                close(fd);
                return 0;
            }
            char item[64] = "";
            if (h->nlmsg_type == RTM_NEWLINK) {
                struct ifinfomsg *msg = NLMSG_DATA(h);
                int len = (int) IFLA_PAYLOAD(h);
                for (struct rtattr *a = IFLA_RTA(msg); RTA_OK(a, len); a = RTA_NEXT(a, len))
                    if (a->rta_type == IFLA_IFNAME)
                        snprintf(item, sizeof(item), "%s;", (char *) RTA_DATA(a));
            } else if (h->nlmsg_type == RTM_NEWADDR) {
                struct ifaddrmsg *msg = NLMSG_DATA(h);
                int len = (int) IFA_PAYLOAD(h);
                for (struct rtattr *a = IFA_RTA(msg); RTA_OK(a, len); a = RTA_NEXT(a, len))
                    if (a->rta_type == IFA_LOCAL) {
                        char ip[32];
                        inet_ntop(AF_INET, RTA_DATA(a), ip, sizeof(ip));
                        snprintf(item, sizeof(item), "%s/%d;", ip, msg->ifa_prefixlen);
                    }
            } else if (h->nlmsg_type == RTM_NEWROUTE) {
                struct rtmsg *msg = NLMSG_DATA(h);
                if (msg->rtm_table != RT_TABLE_MAIN)
                    continue;
                int len = (int) RTM_PAYLOAD(h);
                char ip[32] = "0.0.0.0";
                int oif = 0;
                for (struct rtattr *a = RTM_RTA(msg); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
                    if (a->rta_type == RTA_DST)
                        inet_ntop(AF_INET, RTA_DATA(a), ip, sizeof(ip));
                    if (a->rta_type == RTA_OIF)
                        oif = *(int *) RTA_DATA(a);
                }
                snprintf(item, sizeof(item), "%s/%d@%d;", ip, msg->rtm_dst_len, oif);
            }
            if (strlen(out) + strlen(item) + 1 < size)
                strcat(out, item);
        }
    }
    close(fd);
    return 0;
}

static int has(int type, const char *item) {
    char all[4096];
    if (dump(type, all, sizeof(all)) < 0)
        return -1;
    char want[80];
    snprintf(want, sizeof(want), "%s;", item);
    return strstr(all, want) != NULL;
}

static struct sockaddr_in in4(const char *ip, int port) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t) port)};
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

static const char *ip_of(const struct sockaddr_in *a) {
    static char text[4][32];
    static int next;
    char *out = text[next++ % 4];
    inet_ntop(AF_INET, &a->sin_addr, out, 32);
    return out;
}

// ---- the namespace over there ----------------------------------------------

// The child: its own network namespace, then whatever it is told, one
// command a line on `cmd`, answering on `ans`.
static int cmd_fd = -1, ans_fd = -1;
static pid_t there;

static void say(const char *text) {
    char line[256];
    snprintf(line, sizeof(line), "%s\n", text);
    MUST(write(cmd_fd, line, strlen(line)) == (ssize_t) strlen(line), "write to the child");
}

static void hear(char *out, size_t size) {
    size_t n = 0;
    while (n + 1 < size) {
        char c;
        if (read(ans_fd, &c, 1) != 1)
            break;
        if (c == '\n')
            break;
        out[n++] = c;
    }
    out[n] = '\0';
}

static void child_loop(int in, int out) {
    MUST(unshare(CLONE_NEWNET) == 0, "the child's unshare(CLONE_NEWNET) failed");
    FILE *f = fdopen(in, "r");
    char line[256];
    int listener = -1, udp = -1, scoped = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        char answer[256] = "ok";
        line[strcspn(line, "\n")] = '\0';
        if (strcmp(line, "setup") == 0) {
            int e = link_up("lo");
            if (e == 0) e = addr_add("vb", "10.200.0.2", 24);
            if (e == 0) e = link_up("vb");
            snprintf(answer, sizeof(answer), "%d", e);
        } else if (strncmp(line, "has-link ", 9) == 0) {
            snprintf(answer, sizeof(answer), "%d", has(RTM_GETLINK, line + 9));
        } else if (strncmp(line, "has-addr ", 9) == 0) {
            snprintf(answer, sizeof(answer), "%d", has(RTM_GETADDR, line + 9));
        } else if (strcmp(line, "listen") == 0) {
            listener = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in a = in4("0.0.0.0", 7400);
            int one = 1;
            setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            int e = bind(listener, (struct sockaddr *) &a, sizeof(a)) == 0 && listen(listener, 4) == 0 ? 0 : errno;
            scoped = socket(AF_INET, SOCK_STREAM, 0);
            a = in4("127.0.0.1", 7401);
            if (e == 0) e = bind(scoped, (struct sockaddr *) &a, sizeof(a)) == 0 && listen(scoped, 4) == 0 ? 0 : errno;
            udp = socket(AF_INET, SOCK_DGRAM, 0);
            a = in4("10.200.0.2", 7402);
            if (e == 0) e = bind(udp, (struct sockaddr *) &a, sizeof(a)) == 0 ? 0 : errno;
            snprintf(answer, sizeof(answer), "%d", e);
        } else if (strcmp(line, "accept") == 0) {
            struct sockaddr_in peer = {0}, mine = {0};
            socklen_t len = sizeof(peer), mlen = sizeof(mine);
            int c = accept(listener, (struct sockaddr *) &peer, &len);
            char data[16] = "";
            ssize_t n = c >= 0 ? read(c, data, sizeof(data) - 1) : -1;
            if (n > 0)
                data[n] = '\0';
            getsockname(c, (struct sockaddr *) &mine, &mlen);
            if (c >= 0 && write(c, "pong", 4) != 4) {}
            snprintf(answer, sizeof(answer), "%s %s %s", c >= 0 ? ip_of(&peer) : "-", mine.sin_family ? ip_of(&mine) : "-", n > 0 ? data : "-");
            if (c >= 0)
                close(c);
        } else if (strcmp(line, "udp") == 0) {
            struct timeval wait = {.tv_sec = 5};
            setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
            struct sockaddr_in from = {0};
            socklen_t len = sizeof(from);
            char data[16] = "";
            ssize_t n = recvfrom(udp, data, sizeof(data) - 1, 0, (struct sockaddr *) &from, &len);
            if (n > 0) {
                data[n] = '\0';
                sendto(udp, "back", 4, 0, (struct sockaddr *) &from, len);
            }
            snprintf(answer, sizeof(answer), "%s %s", n > 0 ? ip_of(&from) : "-", n > 0 ? data : "-");
        } else if (strcmp(line, "exit") == 0) {
            break;
        }
        char reply[300];
        snprintf(reply, sizeof(reply), "%s\n", answer);
        if (write(out, reply, strlen(reply)) < 0)
            break;
    }
    _exit(0);
}

static uint16_t checksum(const uint8_t *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2)
        sum += (uint32_t) data[i] << 8 | data[i + 1];
    if (len & 1)
        sum += (uint32_t) data[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t) ~sum;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (geteuid() == 0) {
        if (setgroups(0, NULL) != 0 || setresgid(1000, 1000, 1000) != 0 || setresuid(1000, 1000, 1000) != 0) {
            printf(NAME ": FAIL setup: cannot become uid 1000: %s\n", strerror(errno));
            return 1;
        }
    }
    uid_t uid = geteuid();
    gid_t gid = getegid();
    if (unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0) {
        printf(NAME ": SKIP (no user and network namespace for an ordinary user here: %s)\n", strerror(errno));
        return 0;
    }
    char map[64];
    write_file("/proc/self/setgroups", "deny");
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) uid);
    MUST(write_file("/proc/self/uid_map", map) == 0, "uid_map");
    snprintf(map, sizeof(map), "0 %u 1", (unsigned) gid);
    MUST(write_file("/proc/self/gid_map", map) == 0, "gid_map");
    MUST(link_up("lo") == 0, "lo up failed");

    int to_child[2], from_child[2];
    MUST(pipe(to_child) == 0 && pipe(from_child) == 0, "pipe");
    fflush(stdout);
    there = fork();
    if (there == 0) {
        close(to_child[1]);
        close(from_child[0]);
        child_loop(to_child[0], from_child[1]);
    }
    close(to_child[0]);
    close(from_child[1]);
    cmd_fd = to_child[1];
    ans_fd = from_child[0];
    char answer[256];
    say("has-link lo");     // (waits for the child's unshare)
    hear(answer, sizeof(answer));

    // ---- create
    int e = veth_add("va", "vb", there);
    MUST(e == 0, "RTM_NEWLINK veth va peer vb in the child's namespace failed: %d", e);
    CHECK(if_nametoindex("va") != 0, "no \"va\" here");
    CHECK(has(RTM_GETLINK, "va") == 1 && has(RTM_GETLINK, "vb") == 0, "here the links are not va without vb");
    say("has-link vb");
    hear(answer, sizeof(answer));
    CHECK(strcmp(answer, "1") == 0, "\"vb\" is not in the child's namespace (%s)", answer);
    say("has-link va");
    hear(answer, sizeof(answer));
    CHECK(strcmp(answer, "0") == 0, "\"va\" is in the child's namespace too (%s)", answer);

    // ---- addresses
    CHECK(addr_add("va", "10.200.0.1", 24) == 0 && link_up("va") == 0, "address and up on va failed");
    say("setup");
    hear(answer, sizeof(answer));
    CHECK(strcmp(answer, "0") == 0, "the child's address and up on vb failed: %s", answer);
    CHECK(has(RTM_GETADDR, "10.200.0.1/24") == 1, "here the address dump has no 10.200.0.1/24");
    say("has-addr 10.200.0.2/24");
    hear(answer, sizeof(answer));
    CHECK(strcmp(answer, "1") == 0, "the child's address dump has no 10.200.0.2/24 (%s)", answer);
    char route[32];
    snprintf(route, sizeof(route), "10.200.0.0/24@%u", if_nametoindex("va"));
    CHECK(has(RTM_GETROUTE, route) == 1, "here the route dump has no 10.200.0.0/24 on va");

    // ---- tcp, udp, scoped
    say("listen");
    hear(answer, sizeof(answer));
    MUST(strcmp(answer, "0") == 0, "the child's sockets failed: %s", answer);
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in far = in4("10.200.0.2", 7400), mine = {0};
    socklen_t mlen = sizeof(mine);
    CHECK(connect(c, (struct sockaddr *) &far, sizeof(far)) == 0, "connect to 10.200.0.2 across the veth failed");
    CHECK(getsockname(c, (struct sockaddr *) &mine, &mlen) == 0 && strcmp(ip_of(&mine), "10.200.0.1") == 0,
          "the client's own address reads %s", ip_of(&mine));
    CHECK(write(c, "ping", 4) == 4, "write");
    say("accept");
    hear(answer, sizeof(answer));
    CHECK(strcmp(answer, "10.200.0.1 10.200.0.2 ping") == 0, "the listener says \"%s\", wanted its peer 10.200.0.1, itself 10.200.0.2, and the data", answer);
    char back[8] = "";
    CHECK(read(c, back, 4) == 4 && memcmp(back, "pong", 4) == 0, "the reply did not come back");
    close(c);

    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval wait = {.tv_sec = 5};
    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    struct sockaddr_in udp_far = in4("10.200.0.2", 7402);
    CHECK(sendto(u, "dgram", 5, 0, (struct sockaddr *) &udp_far, sizeof(udp_far)) == 5, "sendto across the veth failed");
    say("udp");
    hear(answer, sizeof(answer));
    CHECK(strcmp(answer, "10.200.0.1 dgram") == 0, "the child got \"%s\"", answer);
    struct sockaddr_in from = {0};
    socklen_t flen = sizeof(from);
    char got[8] = "";
    ssize_t n = recvfrom(u, got, sizeof(got), 0, (struct sockaddr *) &from, &flen);
    CHECK(n == 4 && memcmp(got, "back", 4) == 0 && strcmp(ip_of(&from), "10.200.0.2") == 0 && ntohs(from.sin_port) == 7402,
          "the reply was %zd bytes from %s:%d", n, ip_of(&from), ntohs(from.sin_port));
    close(u);

    c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in scoped = in4("10.200.0.2", 7401);
    errno = 0;
    CHECK(connect(c, (struct sockaddr *) &scoped, sizeof(scoped)) == -1 && errno == ECONNREFUSED,
          "a listener there on 127.0.0.1 was reached at 10.200.0.2 (or failed some other way)");
    close(c);
    c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in nowhere = in4("10.201.0.1", 7400);
    errno = 0;
    CHECK(connect(c, (struct sockaddr *) &nowhere, sizeof(nowhere)) == -1 && errno == ENETUNREACH,
          "a connect to an address with no route did not fail with ENETUNREACH");
    close(c);

    // ---- ping
    int raw = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    CHECK(raw >= 0, "a raw ICMP socket failed");
    if (raw >= 0) {
        setsockopt(raw, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
        uint8_t echo[16] = {8, 0, 0, 0, 0x12, 0x34, 0, 1, 'v', 'e', 't', 'h', 'p', 'i', 'n', 'g'};
        uint16_t sum = checksum(echo, sizeof(echo));
        echo[2] = (uint8_t) (sum >> 8);
        echo[3] = (uint8_t) sum;
        CHECK(sendto(raw, echo, sizeof(echo), 0, (struct sockaddr *) &far, sizeof(far)) == sizeof(echo), "the echo request was not sent");
        uint8_t reply[128];
        struct sockaddr_in rfrom = {0};
        socklen_t rlen = sizeof(rfrom);
        ssize_t rn = recvfrom(raw, reply, sizeof(reply), 0, (struct sockaddr *) &rfrom, &rlen);
        CHECK(rn == 20 + (ssize_t) sizeof(echo) && reply[20] == 0 && strcmp(ip_of(&rfrom), "10.200.0.2") == 0,
              "the first ICMP message back is %zd bytes, type %d, from %s; wanted the echo reply from 10.200.0.2", rn, rn > 20 ? reply[20] : -1, ip_of(&rfrom));
        close(raw);
    }

    // ---- move, delete
    CHECK(veth_add("vc", "vd", 0) == 0, "a pair with both ends here failed");
    CHECK(has(RTM_GETLINK, "vc") == 1 && has(RTM_GETLINK, "vd") == 1, "the pair is not both here");
    CHECK(link_to_ns("vd", there) == 0, "moving vd to the child's namespace failed");
    CHECK(has(RTM_GETLINK, "vd") == 0, "vd is still here after the move");
    say("has-link vd");
    hear(answer, sizeof(answer));
    CHECK(strcmp(answer, "1") == 0, "vd is not in the child's namespace after the move (%s)", answer);
    CHECK(link_del("vc") == 0, "deleting vc failed");
    say("has-link vd");
    hear(answer, sizeof(answer));
    CHECK(strcmp(answer, "0") == 0, "deleting one end left the other (%s)", answer);

    // ---- dummy
    CHECK(link_add_kind("dm0", "dummy") == 0 && addr_add("dm0", "10.9.9.9", 32) == 0 && link_up("dm0") == 0, "the dummy link failed");
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in dm = in4("10.9.9.9", 7410);
    CHECK(bind(l, (struct sockaddr *) &dm, sizeof(dm)) == 0 && listen(l, 1) == 0, "a listener on the dummy's address failed");
    c = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *) &dm, sizeof(dm)) == 0, "a connect to the dummy's address from its own namespace failed");
    close(c);
    close(l);

    say("exit");
    int status = -1;
    waitpid(there, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child: status %#x", status);
    if (failures != 0) {
        printf(NAME ": FAIL failures=%d\n", failures);
        return 1;
    }
    printf(NAME ": PASS\n");
    return 0;
}
