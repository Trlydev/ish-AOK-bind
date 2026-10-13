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
// bubblewrap --unshare-net does), and through which nothing travels.
//
// A socket belongs to the namespace of the task that made it, for its whole
// life, wherever the descriptor goes afterwards (fd->socket.net_ns; an
// accepted socket is its listener's). For a socket of a private namespace:
//
//   - an inet socket never touches the host's network. bind() is recorded and
//     reported back, to loopback and wildcard addresses only, and nothing is
//     bound on the host. connect() and a send with a destination are
//     ENETUNREACH, or for a loopback destination ECONNREFUSED -- which is the
//     truth, because listen() is refused (EOPNOTSUPP): there is no loopback
//     traffic between sockets yet, and a server that could never be reached
//     is told so where it asks, not left waiting.
//   - an abstract AF_UNIX name is the namespace's own: it neither finds nor
//     collides with one bound outside. Pathname sockets are the
//     filesystem's, as on Linux.
//   - netlink: the route family answers for "lo" alone and takes RTM_NEWADDR
//     and RTM_NEWLINK/SETLINK for it from whoever has CAP_NET_ADMIN over the
//     namespace; nothing is pushed to it about the host's interfaces; the
//     socket-diag family lists nothing.
//   - the interface ioctls know "lo" and nothing else.
//
// What a private namespace does not yet do is carry loopback traffic, and
// /proc/net and /sys/class/net still describe the machine's network to
// whoever reads them.
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
