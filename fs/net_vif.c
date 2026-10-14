// Virtual network interfaces: see fs/net_vif.h.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/errno.h"
#include "util/list.h"
#include "util/sync.h"
#include "fs/net_vif.h"

struct net_vif {
    struct list links;          // vifs, under vif_lock
    struct net_namespace *ns;   // NULL: the initial namespace. Not a reference:
                                // the namespace takes its interfaces with it
                                // when it goes (net_vif_namespace_gone)
    int index;
    char name[NET_VIF_NAME_MAX];
    enum net_vif_kind kind;
    bool up;
    unsigned mtu;
    uint8_t mac[6];
    struct net_vif *peer;       // a veth's other end
    int n_addrs;
    struct net_vif_addr addrs[NET_VIF_MAX_ADDRS];
    uint64_t rx_packets, rx_bytes, tx_packets, tx_bytes;
};

struct net_vif_route_entry {
    struct list links;
    struct net_namespace *ns;
    struct net_vif_route route;
};

static lock_t vif_lock = LOCK_INITIALIZER;
static struct list vifs = LIST_INITIALIZER(vifs);
static struct list vif_routes = LIST_INITIALIZER(vif_routes);
static atomic_int vif_count;

bool net_vif_any(void) {
    return atomic_load_explicit(&vif_count, memory_order_relaxed) != 0;
}

static struct net_vif *vif_find_locked(struct net_namespace *ns, int index, const char *name) {
    struct net_vif *vif;
    list_for_each_entry(&vifs, vif, links) {
        if (vif->ns != ns)
            continue;
        if (index != 0 ? vif->index == index : (name != NULL && strcmp(vif->name, name) == 0))
            return vif;
    }
    return NULL;
}

// The next free index in `ns`: after "lo" (1) in a private namespace; well
// clear of the host's own in the initial one.
static int vif_next_index_locked(struct net_namespace *ns) {
    int index = ns == NULL ? 4096 : 2;
    struct net_vif *vif;
    bool taken = true;
    while (taken) {
        taken = false;
        list_for_each_entry(&vifs, vif, links) {
            if (vif->ns == ns && vif->index == index) {
                index++;
                taken = true;
                break;
            }
        }
    }
    return index;
}

static bool vif_name_ok(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len >= NET_VIF_NAME_MAX || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (size_t i = 0; i < len; i++)
        if (name[i] == '/' || name[i] == ':' || name[i] == ' ' || name[i] == '\t' || name[i] == '\n')
            return false;
    return true;
}

// A name for an end nobody named: the kind and the first free number, as
// Linux makes "veth0".
static void vif_make_name_locked(struct net_namespace *ns, const char *base, char *out) {
    for (int n = 0; n < 10000; n++) {
        snprintf(out, NET_VIF_NAME_MAX, "%s%d", base, n);
        if (vif_find_locked(ns, 0, out) == NULL)
            return;
    }
}

static void vif_random_mac(uint8_t mac[6]) {
    arc4random_buf(mac, 6);
    mac[0] = (uint8_t) ((mac[0] & 0xfe) | 0x02);    // unicast, locally administered
}

static struct net_vif *vif_new_locked(struct net_namespace *ns, enum net_vif_kind kind, const char *name) {
    struct net_vif *vif = calloc(1, sizeof(*vif));
    if (vif == NULL)
        return NULL;
    vif->ns = ns;
    vif->index = vif_next_index_locked(ns);
    snprintf(vif->name, sizeof(vif->name), "%s", name);
    vif->kind = kind;
    vif->mtu = 1500;
    vif_random_mac(vif->mac);
    list_add_tail(&vifs, &vif->links);
    atomic_fetch_add(&vif_count, 1);
    return vif;
}

// The routes that name an interface go when it does, or leaves for another
// namespace.
static void vif_routes_drop_locked(struct net_namespace *ns, int index) {
    struct net_vif_route_entry *entry, *tmp;
    list_for_each_entry_safe(&vif_routes, entry, tmp, links) {
        if (entry->ns == ns && entry->route.oif == index) {
            list_remove(&entry->links);
            free(entry);
        }
    }
}

static void vif_free_locked(struct net_vif *vif) {
    vif_routes_drop_locked(vif->ns, vif->index);
    list_remove(&vif->links);
    atomic_fetch_sub(&vif_count, 1);
    free(vif);
}

static void vif_info(const struct net_vif *vif, struct net_vif_info *out) {
    memset(out, 0, sizeof(*out));
    out->index = vif->index;
    memcpy(out->name, vif->name, sizeof(out->name));
    out->kind = vif->kind;
    out->up = vif->up;
    out->lower_up = vif->up && (vif->kind != NET_VIF_VETH || (vif->peer != NULL && vif->peer->up));
    out->mtu = vif->mtu;
    memcpy(out->mac, vif->mac, sizeof(out->mac));
    if (vif->peer != NULL) {
        out->peer_index = vif->peer->index;
        out->peer_here = vif->peer->ns == vif->ns;
    }
    out->n_addrs = vif->n_addrs;
    memcpy(out->addrs, vif->addrs, sizeof(out->addrs));
    out->rx_packets = vif->rx_packets;
    out->rx_bytes = vif->rx_bytes;
    out->tx_packets = vif->tx_packets;
    out->tx_bytes = vif->tx_bytes;
}

int net_vif_list(struct net_namespace *ns, struct net_vif_info *out, int max) {
    if (!net_vif_any())
        return 0;
    int count = 0;
    lock(&vif_lock, 0);
    struct net_vif *vif;
    list_for_each_entry(&vifs, vif, links) {
        if (vif->ns != ns)
            continue;
        if (out != NULL && count < max)
            vif_info(vif, &out[count]);
        count++;
    }
    unlock(&vif_lock);
    return count;
}

int net_vif_get(struct net_namespace *ns, int index, const char *name, struct net_vif_info *out) {
    if (!net_vif_any())
        return _ENODEV;
    lock(&vif_lock, 0);
    struct net_vif *vif = vif_find_locked(ns, index, name);
    if (vif != NULL && out != NULL)
        vif_info(vif, out);
    unlock(&vif_lock);
    return vif != NULL ? 0 : _ENODEV;
}

// The subnet an address and prefix name.
static void addr_mask(bool v6, const uint8_t *in, uint8_t prefix, uint8_t *out) {
    int bytes = v6 ? 16 : 4;
    memset(out, 0, 16);
    for (int i = 0; i < bytes; i++) {
        int bits = (int) prefix - i * 8;
        uint8_t mask = bits >= 8 ? 0xff : bits <= 0 ? 0 : (uint8_t) (0xff << (8 - bits));
        out[i] = in[i] & mask;
    }
}

static bool addr_in(bool v6, const uint8_t *addr, const uint8_t *net, uint8_t prefix) {
    uint8_t a[16], b[16];
    addr_mask(v6, addr, prefix, a);
    addr_mask(v6, net, prefix, b);
    return memcmp(a, b, v6 ? 16 : 4) == 0;
}

static bool addr_eq(bool v6, const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, v6 ? 16 : 4) == 0;
}

int net_vif_routes(struct net_namespace *ns, struct net_vif_route *out, int max) {
    if (!net_vif_any())
        return 0;
    int count = 0;
    lock(&vif_lock, 0);
    struct net_vif *vif;
    list_for_each_entry(&vifs, vif, links) {
        if (vif->ns != ns || !vif->up)
            continue;
        for (int i = 0; i < vif->n_addrs; i++) {
            if (out != NULL && count < max) {
                struct net_vif_route *route = &out[count];
                memset(route, 0, sizeof(*route));
                route->v6 = vif->addrs[i].v6;
                route->prefix = vif->addrs[i].prefix;
                addr_mask(route->v6, vif->addrs[i].addr, route->prefix, route->dst);
                route->oif = vif->index;
                route->connected = true;
            }
            count++;
        }
    }
    struct net_vif_route_entry *entry;
    list_for_each_entry(&vif_routes, entry, links) {
        if (entry->ns != ns)
            continue;
        if (out != NULL && count < max)
            out[count] = entry->route;
        count++;
    }
    unlock(&vif_lock);
    return count;
}

int net_vif_create(struct net_namespace *ns, enum net_vif_kind kind, const char *name,
        const char *peer_name, struct net_namespace *peer_ns) {
    if (kind != NET_VIF_VETH && kind != NET_VIF_DUMMY)
        return _EOPNOTSUPP;
    lock(&vif_lock, 0);
    char own[NET_VIF_NAME_MAX], other[NET_VIF_NAME_MAX];
    if (name != NULL && name[0] != '\0')
        snprintf(own, sizeof(own), "%s", name);
    else
        vif_make_name_locked(ns, kind == NET_VIF_VETH ? "veth" : "dummy", own);
    if (!vif_name_ok(own)) {
        unlock(&vif_lock);
        return _EINVAL;
    }
    if (vif_find_locked(ns, 0, own) != NULL) {
        unlock(&vif_lock);
        return _EEXIST;
    }
    if (kind == NET_VIF_VETH) {
        if (peer_name != NULL && peer_name[0] != '\0') {
            snprintf(other, sizeof(other), "%s", peer_name);
        } else {
            vif_make_name_locked(peer_ns, "veth", other);
            // (Not the near end's name, in one namespace.)
            if (peer_ns == ns && strcmp(other, own) == 0) {
                char base[NET_VIF_NAME_MAX];
                snprintf(base, sizeof(base), "%.10sp", own);
                vif_make_name_locked(peer_ns, base, other);
            }
        }
        if (!vif_name_ok(other)) {
            unlock(&vif_lock);
            return _EINVAL;
        }
        if (vif_find_locked(peer_ns, 0, other) != NULL || (peer_ns == ns && strcmp(other, own) == 0)) {
            unlock(&vif_lock);
            return _EEXIST;
        }
    }
    struct net_vif *vif = vif_new_locked(ns, kind, own);
    if (vif == NULL) {
        unlock(&vif_lock);
        return _ENOMEM;
    }
    if (kind == NET_VIF_VETH) {
        struct net_vif *peer = vif_new_locked(peer_ns, kind, other);
        if (peer == NULL) {
            vif_free_locked(vif);
            unlock(&vif_lock);
            return _ENOMEM;
        }
        vif->peer = peer;
        peer->peer = vif;
    }
    unlock(&vif_lock);
    return 0;
}

int net_vif_delete(struct net_namespace *ns, int index) {
    lock(&vif_lock, 0);
    struct net_vif *vif = vif_find_locked(ns, index, NULL);
    if (vif == NULL) {
        unlock(&vif_lock);
        return _ENODEV;
    }
    if (vif->peer != NULL)
        vif_free_locked(vif->peer);
    vif_free_locked(vif);
    unlock(&vif_lock);
    return 0;
}

int net_vif_change(struct net_namespace *ns, int index, const struct net_vif_change *change) {
    lock(&vif_lock, 0);
    struct net_vif *vif = vif_find_locked(ns, index, NULL);
    if (vif == NULL) {
        unlock(&vif_lock);
        return _ENODEV;
    }
    struct net_namespace *where = change->move ? change->to_ns : ns;
    const char *name = change->name != NULL && change->name[0] != '\0' ? change->name : vif->name;
    if (!vif_name_ok(name)) {
        unlock(&vif_lock);
        return _EINVAL;
    }
    struct net_vif *clash = vif_find_locked(where, 0, name);
    if (clash != NULL && clash != vif) {
        unlock(&vif_lock);
        return _EEXIST;
    }
    if (change->mtu != 0 && (change->mtu < 68 || change->mtu > 65535)) {
        unlock(&vif_lock);
        return _EINVAL;
    }
    snprintf(vif->name, sizeof(vif->name), "%s", name);
    if (change->mtu != 0)
        vif->mtu = change->mtu;
    if (change->mac != NULL)
        memcpy(vif->mac, change->mac, 6);
    if (change->move && change->to_ns != ns) {
        // Into another namespace it goes down, without its addresses and
        // routes, as on Linux; it keeps its index if that is free there.
        vif_routes_drop_locked(ns, vif->index);
        vif->up = false;
        vif->n_addrs = 0;
        int old_index = vif->index;
        vif->ns = change->to_ns;
        vif->index = 0;
        struct net_vif *other;
        bool free_there = true;
        list_for_each_entry(&vifs, other, links)
            if (other != vif && other->ns == change->to_ns && other->index == old_index)
                free_there = false;
        if ((change->to_ns == NULL && old_index < 4096) || (change->to_ns != NULL && old_index < 2))
            free_there = false;
        vif->index = free_there ? old_index : vif_next_index_locked(change->to_ns);
    } else if (change->up >= 0) {
        vif->up = change->up != 0;
    }
    unlock(&vif_lock);
    return 0;
}

int net_vif_address(struct net_namespace *ns, int index, bool add, const struct net_vif_addr *addr) {
    if (addr->prefix > (addr->v6 ? 128 : 32))
        return _EINVAL;
    lock(&vif_lock, 0);
    struct net_vif *vif = vif_find_locked(ns, index, NULL);
    if (vif == NULL) {
        unlock(&vif_lock);
        return _ENODEV;
    }
    int found = -1;
    for (int i = 0; i < vif->n_addrs; i++)
        if (vif->addrs[i].v6 == addr->v6 && addr_eq(addr->v6, vif->addrs[i].addr, addr->addr))
            found = i;
    int err = 0;
    if (add) {
        if (found >= 0)
            err = _EEXIST;
        else if (vif->n_addrs == NET_VIF_MAX_ADDRS)
            err = _ENOSPC;
        else
            vif->addrs[vif->n_addrs++] = *addr;
    } else if (found < 0) {
        err = _EADDRNOTAVAIL;
    } else {
        vif->addrs[found] = vif->addrs[--vif->n_addrs];
    }
    unlock(&vif_lock);
    return err;
}

int net_vif_route_change(struct net_namespace *ns, bool add, const struct net_vif_route *route) {
    if (route->prefix > (route->v6 ? 128 : 32))
        return _EINVAL;
    lock(&vif_lock, 0);
    // A route has to say where: an interface of this namespace, or a gateway
    // one of its connected subnets reaches.
    // (Or a private namespace's "lo", index 1, which reaches only itself.)
    if (add && route->oif != 0 && !(ns != NULL && route->oif == 1) &&
            vif_find_locked(ns, route->oif, NULL) == NULL) {
        unlock(&vif_lock);
        return _ENODEV;
    }
    if (add && route->oif == 0) {
        bool reached = false;
        struct net_vif *vif;
        list_for_each_entry(&vifs, vif, links) {
            if (vif->ns != ns || !route->has_gateway)
                continue;
            for (int i = 0; i < vif->n_addrs && !reached; i++)
                reached = vif->addrs[i].v6 == route->v6 &&
                    addr_in(route->v6, route->gateway, vif->addrs[i].addr, vif->addrs[i].prefix);
        }
        if (!reached) {
            unlock(&vif_lock);
            return _ENETUNREACH;
        }
    }
    struct net_vif_route_entry *entry, *tmp, *found = NULL;
    list_for_each_entry_safe(&vif_routes, entry, tmp, links) {
        if (entry->ns == ns && entry->route.v6 == route->v6 && entry->route.prefix == route->prefix &&
                addr_eq(route->v6, entry->route.dst, route->dst)) {
            found = entry;
            break;
        }
    }
    int err = 0;
    if (add) {
        if (found != NULL) {
            err = _EEXIST;
        } else if ((entry = calloc(1, sizeof(*entry))) == NULL) {
            err = _ENOMEM;
        } else {
            entry->ns = ns;
            entry->route = *route;
            addr_mask(route->v6, route->dst, route->prefix, entry->route.dst);
            entry->route.connected = false;
            list_add_tail(&vif_routes, &entry->links);
        }
    } else if (found == NULL) {
        err = _ESRCH;
    } else {
        list_remove(&found->links);
        free(found);
    }
    unlock(&vif_lock);
    return err;
}

void net_vif_namespace_gone(struct net_namespace *ns) {
    if (ns == NULL || !net_vif_any())
        return;
    lock(&vif_lock, 0);
    bool again = true;
    while (again) {
        again = false;
        struct net_vif *vif;
        list_for_each_entry(&vifs, vif, links) {
            if (vif->ns != ns)
                continue;
            if (vif->peer != NULL)
                vif_free_locked(vif->peer);
            vif_free_locked(vif);
            again = true;
            break;
        }
    }
    struct net_vif_route_entry *entry, *tmp;
    list_for_each_entry_safe(&vif_routes, entry, tmp, links) {
        if (entry->ns == ns) {
            list_remove(&entry->links);
            free(entry);
        }
    }
    unlock(&vif_lock);
}

static bool vif_local_locked(struct net_namespace *ns, bool v6, const uint8_t *addr) {
    struct net_vif *vif;
    list_for_each_entry(&vifs, vif, links) {
        if (vif->ns != ns || !vif->up)
            continue;
        for (int i = 0; i < vif->n_addrs; i++)
            if (vif->addrs[i].v6 == v6 && addr_eq(v6, vif->addrs[i].addr, addr))
                return true;
    }
    return false;
}

bool net_vif_local(struct net_namespace *ns, bool v6, const uint8_t *addr) {
    if (!net_vif_any())
        return false;
    lock(&vif_lock, 0);
    bool local = vif_local_locked(ns, v6, addr);
    unlock(&vif_lock);
    return local;
}

// The first address of `vif` in the family: what leaves through it leaves
// from there.
static bool vif_source(const struct net_vif *vif, bool v6, uint8_t *src) {
    for (int i = 0; i < vif->n_addrs; i++) {
        if (vif->addrs[i].v6 == v6) {
            memcpy(src, vif->addrs[i].addr, 16);
            return true;
        }
    }
    return false;
}

int net_vif_resolve(struct net_namespace *ns, bool v6, const uint8_t *dst,
        struct net_namespace **to, uint8_t *src) {
    if (!net_vif_any())
        return _ENETUNREACH;
    lock(&vif_lock, 0);
    if (vif_local_locked(ns, v6, dst)) {
        *to = ns;
        memcpy(src, dst, 16);
        unlock(&vif_lock);
        return 0;
    }
    // The longest prefix that reaches it: a connected subnet, or a route.
    struct net_vif *best = NULL;
    int best_prefix = -1;
    struct net_vif *vif;
    list_for_each_entry(&vifs, vif, links) {
        if (vif->ns != ns || !vif->up)
            continue;
        for (int i = 0; i < vif->n_addrs; i++) {
            const struct net_vif_addr *a = &vif->addrs[i];
            if (a->v6 == v6 && a->prefix > best_prefix && addr_in(v6, dst, a->addr, a->prefix)) {
                best = vif;
                best_prefix = a->prefix;
            }
        }
    }
    struct net_vif_route_entry *entry;
    list_for_each_entry(&vif_routes, entry, links) {
        const struct net_vif_route *route = &entry->route;
        if (entry->ns != ns || route->v6 != v6 || route->prefix <= best_prefix ||
                !addr_in(v6, dst, route->dst, route->prefix))
            continue;
        struct net_vif *through = route->oif != 0 ? vif_find_locked(ns, route->oif, NULL) : NULL;
        if (through == NULL && route->has_gateway) {
            list_for_each_entry(&vifs, vif, links) {
                if (vif->ns != ns || !vif->up)
                    continue;
                for (int i = 0; i < vif->n_addrs && through == NULL; i++)
                    if (vif->addrs[i].v6 == v6 && addr_in(v6, route->gateway, vif->addrs[i].addr, vif->addrs[i].prefix))
                        through = vif;
            }
        }
        if (through != NULL && through->up) {
            best = through;
            best_prefix = route->prefix;
        }
    }
    if (best == NULL) {
        unlock(&vif_lock);
        return _ENETUNREACH;
    }
    int err = _EHOSTUNREACH;
    struct net_vif *peer = best->peer;
    if (best->kind == NET_VIF_VETH && peer != NULL && peer->up &&
            vif_local_locked(peer->ns, v6, dst) && vif_source(best, v6, src)) {
        *to = peer->ns;
        err = 0;
    }
    unlock(&vif_lock);
    return err;
}
