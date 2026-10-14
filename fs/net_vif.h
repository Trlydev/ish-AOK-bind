#ifndef FS_NET_VIF_H
#define FS_NET_VIF_H

// Virtual network interfaces: veth pairs and dummy links (fs/net_vif.c).
//
// A network namespace here has had "lo" and nothing else (kernel/net_ns.h).
// These are the interfaces a namespace can be given besides: a veth is two
// ends, each in a namespace, and what one end sends the other receives; a
// dummy is an interface that holds addresses and carries nothing. Either may
// be in the initial namespace too, beside the host's own interfaces.
//
// An interface is a name and an index in its namespace, an up flag, an MTU,
// a MAC and addresses; routes are a namespace's own, plus the connected one
// every address on an up interface brings. There is no second network
// behind any of it: what crosses a veth is the namespaces' own traffic, the
// emulated sockets of fs/sock.c talking to each other. Nothing is forwarded
// further than the far end of one veth.

#include <stdbool.h>
#include <stdint.h>

struct net_namespace;

#define NET_VIF_MAX_ADDRS 8
#define NET_VIF_NAME_MAX 16

enum net_vif_kind {
    NET_VIF_VETH = 1,
    NET_VIF_DUMMY = 2,
};

struct net_vif_addr {
    bool v6;
    uint8_t prefix;
    uint8_t addr[16];   // network order; IPv4 in the first four bytes
};

// One interface as a listing shows it: copied out, so nothing is held.
struct net_vif_info {
    int index;
    char name[NET_VIF_NAME_MAX];
    enum net_vif_kind kind;
    bool up;
    bool lower_up;      // up, and (a veth) its peer up too: the carrier
    unsigned mtu;
    uint8_t mac[6];
    int peer_index;     // a veth's other end, in its own namespace's numbers
    bool peer_here;     // ...which is this namespace
    int n_addrs;
    struct net_vif_addr addrs[NET_VIF_MAX_ADDRS];
    uint64_t rx_packets, rx_bytes, tx_packets, tx_bytes;
};

struct net_vif_route {
    bool v6;
    uint8_t prefix;
    uint8_t dst[16];
    bool has_gateway;
    uint8_t gateway[16];
    int oif;            // 0: whichever interface reaches the gateway
    bool connected;     // made by an address, not by a request
};

// Are there any at all? (Everything else is skipped while there are none.)
bool net_vif_any(void);

// The interfaces of `ns` (NULL: the initial one), up to `max` into `out`;
// returns how many there are.
int net_vif_list(struct net_namespace *ns, struct net_vif_info *out, int max);
// One, by index (or by name when index is 0): 0, or _ENODEV.
int net_vif_get(struct net_namespace *ns, int index, const char *name, struct net_vif_info *out);
// The routes of `ns`: connected ones first.
int net_vif_routes(struct net_namespace *ns, struct net_vif_route *out, int max);

// RTM_NEWLINK with a kind: a dummy, or a veth whose peer is named
// `peer_name` (one is made up when NULL) in `peer_ns`. 0 or a negative
// errno: EEXIST for a name taken, EINVAL for a bad one.
int net_vif_create(struct net_namespace *ns, enum net_vif_kind kind, const char *name,
        const char *peer_name, struct net_namespace *peer_ns);
// RTM_DELLINK: a veth goes with its peer.
int net_vif_delete(struct net_namespace *ns, int index);

struct net_vif_change {
    const char *name;           // NULL: as it is
    int up;                     // -1: as it is
    unsigned mtu;               // 0: as it is
    const uint8_t *mac;         // NULL: as it is
    bool move;                  // to `to_ns`
    struct net_namespace *to_ns;
};
int net_vif_change(struct net_namespace *ns, int index, const struct net_vif_change *change);

// RTM_NEWADDR / RTM_DELADDR.
int net_vif_address(struct net_namespace *ns, int index, bool add, const struct net_vif_addr *addr);
// RTM_NEWROUTE / RTM_DELROUTE.
int net_vif_route_change(struct net_namespace *ns, bool add, const struct net_vif_route *route);

// The last reference to `ns` is going: its interfaces, and a veth's peer
// wherever it is, go with it.
void net_vif_namespace_gone(struct net_namespace *ns);

// Is `addr` an address of one of `ns`'s up interfaces?
bool net_vif_local(struct net_namespace *ns, bool v6, const uint8_t *addr);

// Where a packet from `ns` to `dst` goes: 0, with *to the namespace whose
// address it is (`ns` itself for one of its own) and `src` (16 bytes) the
// address it leaves from; or _ENETUNREACH with no route, _EHOSTUNREACH when
// the route leads nowhere that has the address (a dummy, a veth whose far
// end is down or is not it).
int net_vif_resolve(struct net_namespace *ns, bool v6, const uint8_t *dst,
        struct net_namespace **to, uint8_t *src);

#endif
