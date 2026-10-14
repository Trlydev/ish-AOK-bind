#ifndef KERNEL_NET_NS_H
#define KERNEL_NET_NS_H

#include <stdatomic.h>
#include <stdbool.h>
#include "misc.h"

struct user_namespace;
struct fd;

// A network namespace (CLONE_NEWNET): a network of a process's own.
//
// AOK's sockets are the host's, and the host has one network. So what a
// private network namespace gets here is the network a sandbox is given so
// that it has none: one interface, "lo" (index 1), which comes up and takes
// 127.0.0.1 and ::1 when it is told to over rtnetlink (which is all
// bubblewrap --unshare-net does), and through which only the namespace's own
// traffic travels.
//
// A socket belongs to the namespace of the task that made it, for its whole
// life, wherever the descriptor goes afterwards (fd->socket.net_ns; an
// accepted socket is its listener's). For a socket of a private namespace:
//
//   - an inet socket never touches the host's network. A TCP or UDP socket
//     is not the host's TCP or UDP at all: its host socket is an AF_UNIX one,
//     a port is a binding in a table of the namespace's own, and loopback
//     traffic between the namespace's sockets goes from one such socket to
//     another (fs/sock.c, "Loopback in a private network namespace"). So a
//     server there listens, a client connects to it, datagrams are answered
//     -- and a destination that is neither the namespace's loopback nor
//     reached through one of its links is ENETUNREACH, because there is
//     nowhere for it to go. A raw ICMP
//     socket's echo requests to the loopback are answered by the kernel,
//     so `ping 127.0.0.1` works; any other raw socket can send nothing.
//   - an abstract AF_UNIX name is the namespace's own: it neither finds nor
//     collides with one bound outside. Pathname sockets are the
//     filesystem's, as on Linux.
//   - netlink: the route family answers for "lo" and the namespace's veth
//     and dummy links (fs/net_vif.h), and takes RTM_NEWLINK/SETLINK/DELLINK,
//     RTM_NEWADDR/DELADDR and RTM_NEWROUTE/DELROUTE for them from whoever has
//     CAP_NET_ADMIN over the namespace; nothing is pushed to it, about the
//     host's interfaces or its own; the socket-diag family lists nothing.
//   - the interface ioctls know "lo" and those links.
//
// A veth's two ends may be in two private namespaces (never the initial
// one), and then a TCP or UDP socket in one reaches a listener in the other
// at an address the far end has, and ping is answered from there: the
// emulated sockets above, with each binding knowing its address and the
// namespace a destination resolves to (net_vif_resolve). Nothing is
// forwarded beyond the far end of one veth.
//
// /proc/net and /sys/class/net describe the reader's own namespace, like
// everything else here that lists interfaces.
//
// The initial namespace is `task->net_ns == NULL` and `socket.net_ns == NULL`.
struct net_namespace {
    atomic_uint refcount;
    unsigned long inode;            // its identity in /proc/<pid>/ns/net
    struct user_namespace *user_ns; // the user namespace it was made in. Owns a reference.
    atomic_bool lo_up;              // IFF_UP on "lo"
    atomic_bool lo_addr4, lo_addr6; // 127.0.0.1/8 and ::1/128 assigned
};

// NULL-is-initial pointers throughout: retain and release accept NULL.
struct net_namespace *net_ns_retain(struct net_namespace *ns);
void net_ns_release(struct net_namespace *ns);
// A new one, empty and down, owned by `owner`; NULL when out of memory.
struct net_namespace *net_ns_new(struct user_namespace *owner);
// The calling task's, NULL for the initial one (and off a task).
struct net_namespace *current_net_ns(void);
// May the calling task use the machine's network through something that is
// not one of its sockets (the native curl's host HTTP session)? Not from a
// private namespace.
bool current_net_is_host(void);

#endif
