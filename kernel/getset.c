#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/personality.h"
#include "kernel/user_ns.h"
#include "kernel/pid_ns.h"
#include "kernel/mnt_ns.h"
#include "kernel/ipc_ns.h" // ns_alloc_inum
#include "fs/proc.h"
#include "fs/stat.h"
#include "util/sync.h"

#define _LINUX_CAPABILITY_VERSION_1_ 0x19980330
#define _LINUX_CAPABILITY_VERSION_2_ 0x20071026
#define _LINUX_CAPABILITY_VERSION_3_ 0x20080522

struct cap_user_header_ {
    dword_t version;
    int_t pid;
};

struct cap_user_data_ {
    dword_t effective;
    dword_t permitted;
    dword_t inheritable;
};

static int cap_data_count(dword_t version) {
    switch (version) {
        case _LINUX_CAPABILITY_VERSION_1_:
            return 1;
        case _LINUX_CAPABILITY_VERSION_2_:
        case _LINUX_CAPABILITY_VERSION_3_:
            return 2;
        default:
            return -1;
    }
}

static bool cap_words_subset(const dword_t *subset, const dword_t *superset, int count) {
    for (int i = 0; i < count; i++) {
        if ((subset[i] & ~superset[i]) != 0)
            return false;
    }
    return true;
}

static bool current_has_cap(uint_t cap) {
    if (current == NULL || cap >= 64)
        return false;
    return (current->cap_effective[cap / 32] & (1u << (cap % 32))) != 0;
}

// Shared with the privileged syscalls elsewhere in the tree; see task.h.
// A capability over the machine: held only in the initial user namespace
// (superuser() says the same of euid 0). What a task holds in a namespace of
// its own is asked with ns_capable() below.
bool current_capable(unsigned cap) {
    return superuser() || (current != NULL && current->user_ns == NULL && current_has_cap(cap));
}

// ---- User namespaces (kernel/user_ns.h) ------------------------------------

struct user_namespace *current_user_ns(void) {
    return current != NULL ? current->user_ns : NULL;
}

struct user_namespace *user_ns_retain(struct user_namespace *ns) {
    if (ns != NULL)
        atomic_fetch_add(&ns->refcount, 1);
    return ns;
}

void user_ns_release(struct user_namespace *ns) {
    // A namespace holds its parent, so the last reference on one can be the
    // last on a chain of them.
    while (ns != NULL && atomic_fetch_sub(&ns->refcount, 1) == 1) {
        struct user_namespace *parent = ns->parent;
        free(ns);
        ns = parent;
    }
}

bool user_ns_in(const struct user_namespace *ns, const struct user_namespace *ancestor) {
    for (;;) {
        if (ns == ancestor)
            return true;
        if (ns == NULL)
            return false;
        ns = ns->parent;
    }
}

static uid_t_ id_map_down(struct id_map *map, uid_t_ id) {
    if (id == USER_NS_INVALID_ID || !atomic_load(&map->set))
        return USER_NS_INVALID_ID;
    for (unsigned i = 0; i < map->count; i++) {
        const struct id_extent *e = &map->extent[i];
        if (id >= e->first && id - e->first < e->count)
            return e->lower + (id - e->first);
    }
    return USER_NS_INVALID_ID;
}

static uid_t_ id_map_up(struct id_map *map, uid_t_ kid) {
    if (kid == USER_NS_INVALID_ID || !atomic_load(&map->set))
        return USER_NS_INVALID_ID;
    for (unsigned i = 0; i < map->count; i++) {
        const struct id_extent *e = &map->extent[i];
        if (kid >= e->lower && kid - e->lower < e->count)
            return e->first + (kid - e->lower);
    }
    return USER_NS_INVALID_ID;
}

uid_t_ user_ns_make_kuid(struct user_namespace *ns, uid_t_ uid) {
    return ns == NULL ? uid : id_map_down(&ns->uid_map, uid);
}
uid_t_ user_ns_make_kgid(struct user_namespace *ns, uid_t_ gid) {
    return ns == NULL ? gid : id_map_down(&ns->gid_map, gid);
}
uid_t_ user_ns_from_kuid(struct user_namespace *ns, uid_t_ kuid) {
    return ns == NULL ? kuid : id_map_up(&ns->uid_map, kuid);
}
uid_t_ user_ns_from_kgid(struct user_namespace *ns, uid_t_ kgid) {
    return ns == NULL ? kgid : id_map_up(&ns->gid_map, kgid);
}
uid_t_ user_ns_from_kuid_munged(struct user_namespace *ns, uid_t_ kuid) {
    uid_t_ uid = user_ns_from_kuid(ns, kuid);
    return uid == USER_NS_INVALID_ID ? USER_NS_OVERFLOW_ID : uid;
}
uid_t_ user_ns_from_kgid_munged(struct user_namespace *ns, uid_t_ kgid) {
    uid_t_ gid = user_ns_from_kgid(ns, kgid);
    return gid == USER_NS_INVALID_ID ? USER_NS_OVERFLOW_ID : gid;
}

uid_t_ current_make_kuid(uid_t_ uid) { return user_ns_make_kuid(current_user_ns(), uid); }
uid_t_ current_make_kgid(uid_t_ gid) { return user_ns_make_kgid(current_user_ns(), gid); }
uid_t_ current_uid_shown(uid_t_ kuid) { return user_ns_from_kuid_munged(current_user_ns(), kuid); }
uid_t_ current_gid_shown(uid_t_ kgid) { return user_ns_from_kgid_munged(current_user_ns(), kgid); }

uid_t_ task_root_kuid(struct task *task) {
    return user_ns_make_kuid(task->user_ns, 0);
}

// Linux's cap_capable, with the initial namespace's own rule (euid 0 holds
// everything there) left as it is.
bool ns_capable(struct user_namespace *target, unsigned cap) {
    if (current == NULL || cap >= 64)
        return false;
    struct user_namespace *mine = current->user_ns;
    unsigned my_level = mine != NULL ? mine->level : 0;
    for (struct user_namespace *ns = target;; ns = ns->parent) {
        if (ns == mine)
            return mine == NULL ? current_capable(cap) : current_has_cap(cap);
        // Not in it and not above it: in a sibling, or below.
        if (ns == NULL || ns->level <= my_level)
            return false;
        // The namespace's creator, in the namespace it was made in, holds
        // every capability over it.
        if (ns->parent == mine && ns->owner == current->euid)
            return true;
    }
}

bool capable_wrt_ids(uid_t_ kuid, uid_t_ kgid, unsigned cap) {
    struct user_namespace *ns = current_user_ns();
    if (ns == NULL)
        return current_capable(cap);
    return user_ns_from_kuid(ns, kuid) != USER_NS_INVALID_ID &&
        user_ns_from_kgid(ns, kgid) != USER_NS_INVALID_ID && ns_capable(ns, cap);
}

bool capable_wrt_inode(const struct statbuf *stat, unsigned cap) {
    return capable_wrt_ids(stat->uid, stat->gid, cap);
}

// Is the calling task's root somewhere other than the root of its mount
// namespace? (Linux's current_chrooted.)
static bool current_chrooted(void) {
    char root[MAX_PATH];
    if (current->fs == NULL || fs_root_path(current->fs, root) < 0)
        return true;
    struct mnt_namespace *mnt_ns = mnt_ns_current();
    if (mnt_ns->root_len == 0)
        return strcmp(root, "/") != 0 && root[0] != '\0';
    return strcmp(root, mnt_ns->root) != 0;
}

struct user_namespace *user_ns_new(void) {
    struct user_namespace *parent = current_user_ns();
    if (parent != NULL && parent->level >= USER_NS_MAX_LEVEL)
        return ERR_PTR(_ENOSPC);
    // A user namespace gives CAP_SYS_CHROOT, and chroot is how a process
    // leaves a chroot it is root in: so not from inside one.
    if (current_chrooted())
        return ERR_PTR(_EPERM);
    // The creator has to be someone in its own namespace, or the new one
    // would be owned by nobody the parent can name.
    if (user_ns_from_kuid(parent, current->euid) == USER_NS_INVALID_ID ||
            user_ns_from_kgid(parent, current->egid) == USER_NS_INVALID_ID)
        return ERR_PTR(_EPERM);
    struct user_namespace *ns = calloc(1, sizeof(*ns));
    if (ns == NULL)
        return ERR_PTR(_ENOMEM);
    atomic_init(&ns->refcount, 1);
    ns->inode = ns_alloc_inum();
    ns->parent = user_ns_retain(parent);
    ns->level = parent != NULL ? parent->level + 1 : 1;
    ns->owner = current->euid;
    ns->group = current->egid;
    // setgroups, once denied, is denied in every namespace made below.
    if (parent != NULL) {
        lock(&parent->lock, 0);
        ns->setgroups_denied = parent->setgroups_denied;
        unlock(&parent->lock);
    }
    lock_init(&ns->lock, "user_ns\0");
    return ns;
}

void user_ns_enter(struct task *task, struct user_namespace *ns) {
    task->user_ns = ns;
    task->cap_permitted[0] = task->cap_effective[0] = task->cap_bounding[0] = CAP_FULL_LOW_;
    task->cap_permitted[1] = task->cap_effective[1] = task->cap_bounding[1] = CAP_FULL_HIGH_;
    task->cap_inheritable[0] = task->cap_inheritable[1] = 0;
    task->cap_ambient[0] = task->cap_ambient[1] = 0;
    task->keepcaps = false;
    // Linux gives the task a new cred object, and whatever compared the old
    // one (a descriptor's opener, fs/fd.c) no longer matches.
    task->exec_gen++;
}

bool user_ns_may_setgroups(struct user_namespace *ns) {
    if (ns == NULL)
        return true;
    lock(&ns->lock, 0);
    bool may = atomic_load(&ns->gid_map.set) && !ns->setgroups_denied;
    unlock(&ns->lock);
    return may;
}

// A map as the reader sees it: each line's third... second column is the
// kernel id in the READER's namespace, or in its parent's when the reader is
// in the namespace itself (Linux's uid_m_show).
void user_ns_show_map(struct user_namespace *ns, bool gid, struct proc_data *buf) {
    if (ns == NULL) {
        proc_printf(buf, "%10u %10u %10u\n", 0u, 0u, 4294967295u);
        return;
    }
    struct id_map *map = gid ? &ns->gid_map : &ns->uid_map;
    if (!atomic_load(&map->set))
        return;
    struct user_namespace *reader = current_user_ns();
    struct user_namespace *lower_ns = reader == ns ? ns->parent : reader;
    for (unsigned i = 0; i < map->count; i++) {
        uid_t_ lower = gid ? user_ns_from_kgid(lower_ns, map->extent[i].lower)
                           : user_ns_from_kuid(lower_ns, map->extent[i].lower);
        proc_printf(buf, "%10u %10u %10u\n", map->extent[i].first, lower, map->extent[i].count);
    }
}

int user_ns_write_map(struct user_namespace *ns, bool gid, const char *text, size_t size) {
    // The initial namespace's map is not for writing.
    if (ns == NULL)
        return _EPERM;
    struct user_namespace *writer = current_user_ns();
    // By a task in the namespace, or in its parent.
    if (writer != ns && writer != ns->parent)
        return _EPERM;
    if (size >= 4096)
        return _EINVAL;
    unsigned cap = gid ? CAP_SETGID_ : CAP_SETUID_;
    if (!ns_capable(ns, CAP_SYS_ADMIN_))
        return _EPERM;

    // "first lower count", a line each.
    struct id_map parsed = {0};
    char *copy = strndup(text, size);
    if (copy == NULL)
        return _ENOMEM;
    int err = 0;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        unsigned long long first, lower, count;
        char extra;
        if (sscanf(line, " %llu %llu %llu %c", &first, &lower, &count, &extra) != 3 ||
                count == 0 || first > 0xfffffffeull || lower > 0xfffffffeull ||
                first + count > 0xffffffffull || lower + count > 0xffffffffull) {
            err = _EINVAL;
            break;
        }
        if (parsed.count == USER_NS_MAP_EXTENTS) {
            err = _EINVAL;
            break;
        }
        // No two lines may cover the same id, on either side.
        for (unsigned i = 0; i < parsed.count; i++) {
            const struct id_extent *e = &parsed.extent[i];
            if ((first < (unsigned long long) e->first + e->count && e->first < first + count) ||
                    (lower < (unsigned long long) e->lower + e->count && e->lower < lower + count))
                err = _EINVAL;
        }
        if (err < 0)
            break;
        parsed.extent[parsed.count++] = (struct id_extent) {(uid_t_) first, (uid_t_) lower, (uid_t_) count};
    }
    free(copy);
    if (err == 0 && parsed.count == 0)
        err = _EINVAL;
    if (err < 0)
        return err;

    lock(&ns->lock, 0);
    struct id_map *map = gid ? &ns->gid_map : &ns->uid_map;
    // Once.
    if (atomic_load(&map->set)) {
        unlock(&ns->lock);
        return _EPERM;
    }
    // What needs no privilege: one id, the writer's own, by the namespace's
    // creator -- and for a group, only once setgroups is denied, or dropping
    // a group would be a way out of a file's group-deny bits. Anything else
    // takes the capability over the PARENT namespace, whose ids these are.
    bool own_id = false;
    if (parsed.count == 1 && parsed.extent[0].count == 1 && ns->owner == current->euid) {
        uid_t_ lower = gid ? user_ns_make_kgid(ns->parent, parsed.extent[0].lower)
                           : user_ns_make_kuid(ns->parent, parsed.extent[0].lower);
        own_id = gid ? (ns->setgroups_denied && lower == current->egid) : lower == current->euid;
    }
    if (!own_id && !ns_capable(ns->parent, cap)) {
        unlock(&ns->lock);
        return _EPERM;
    }
    // The lower ids are the parent's: resolve them to kernel ids now, each
    // range whole.
    for (unsigned i = 0; i < parsed.count; i++) {
        struct id_extent *e = &parsed.extent[i];
        uid_t_ lo = gid ? user_ns_make_kgid(ns->parent, e->lower) : user_ns_make_kuid(ns->parent, e->lower);
        uid_t_ hi = gid ? user_ns_make_kgid(ns->parent, e->lower + e->count - 1)
                        : user_ns_make_kuid(ns->parent, e->lower + e->count - 1);
        if (lo == USER_NS_INVALID_ID || hi == USER_NS_INVALID_ID || hi - lo != e->count - 1) {
            unlock(&ns->lock);
            return _EPERM;
        }
        e->lower = lo;
    }
    map->count = parsed.count;
    memcpy(map->extent, parsed.extent, sizeof(parsed.extent));
    atomic_store(&map->set, true);
    unlock(&ns->lock);
    return 0;
}

void user_ns_show_setgroups(struct user_namespace *ns, struct proc_data *buf) {
    bool denied = false;
    if (ns != NULL) {
        lock(&ns->lock, 0);
        denied = ns->setgroups_denied;
        unlock(&ns->lock);
    }
    proc_printf(buf, "%s\n", denied ? "deny" : "allow");
}

int user_ns_write_setgroups(struct user_namespace *ns, const char *text, size_t size) {
    while (size > 0 && (text[size - 1] == '\n' || text[size - 1] == ' '))
        size--;
    bool allow;
    if (size == 5 && memcmp(text, "allow", 5) == 0)
        allow = true;
    else if (size == 4 && memcmp(text, "deny", 4) == 0)
        allow = false;
    else
        return _EINVAL;
    if (!ns_capable(ns, CAP_SYS_ADMIN_))
        return _EPERM;
    if (ns == NULL)
        return allow ? 0 : _EPERM; // (its gid map has always been written)
    lock(&ns->lock, 0);
    int err = 0;
    if (allow) {
        // Not back on once it is off.
        if (ns->setgroups_denied)
            err = _EPERM;
    } else {
        // Not off once the gid map has turned it on for good.
        if (atomic_load(&ns->gid_map.set) && !ns->setgroups_denied)
            err = _EPERM;
        else
            ns->setgroups_denied = true;
    }
    unlock(&ns->lock);
    return err;
}

bool task_ptrace_may_access(struct task *target, unsigned mode) {
    if (current == NULL || target == NULL)
        return false;
    // Linux's same_thread_group: a process may always look at itself, from
    // any of its threads.
    if (target == current || (target->group != NULL && target->group == current->group))
        return true;
    if (current_capable(CAP_SYS_PTRACE_))
        return true;
    uid_t_ uid = (mode & PTRACE_MODE_FSCREDS_) ? current->fsuid : current->uid;
    uid_t_ gid = (mode & PTRACE_MODE_FSCREDS_) ? current->fsgid : current->gid;
    // Linux requires the caller's id to equal ALL THREE of the target's
    // uids, and the same for gids. Comparing only the effective ids would let
    // a process that has temporarily dropped privilege be read by one that
    // never held any -- it could regain that privilege later, so its memory is
    // still privileged memory.
    if (uid != target->uid || uid != target->euid || uid != target->suid ||
            gid != target->gid || gid != target->egid || gid != target->sgid)
        return false;
    // Same user, but the process asked not to be looked at, or became
    // privileged in a way its user must not see into.
    if (target->group != NULL && atomic_load(&target->group->undumpable))
        return false;
    return true;
}

bool current_may_access_task_mem(struct task *target) {
    return task_ptrace_may_access(target, PTRACE_MODE_ATTACH_ | PTRACE_MODE_FSCREDS_);
}

void cred_change_begin(struct cred_change *change) {
    *change = (struct cred_change) {
        .euid = current->euid,
        .egid = current->egid,
        .fsuid = current->fsuid,
        .fsgid = current->fsgid,
        .cap_permitted = {current->cap_permitted[0], current->cap_permitted[1]},
    };
}

// Linux's commit_creds forgets the parent-death signal when the effective or
// filesystem ids change, or the permitted capabilities grow: the process is
// no longer the one its parent could signal when it asked, and a parent must
// not be able to signal a process that has become privileged. The real and
// saved ids and the supplementary groups do not count. Measured on Linux 6.12
// (camd, as root): setresuid(-1, 1000, -1), setfsuid(1000), the same two for
// the gids, and setuid(1000) forget it; setresuid(1000, -1, -1),
// setresuid(-1, -1, 1000), setgroups, dropping a capability, and setuid to
// the uid it already has keep it. AOK kept it through all of them.
//
// Per thread, as the setting is: libc runs a setuid() on every thread of the
// process, and each forgets its own.
void cred_change_commit(const struct cred_change *change) {
    if (current->euid != change->euid || current->egid != change->egid ||
            current->fsuid != change->fsuid || current->fsgid != change->fsgid ||
            (current->cap_permitted[0] & ~change->cap_permitted[0]) != 0 ||
            (current->cap_permitted[1] & ~change->cap_permitted[1]) != 0) {
        current->pdeath_signal = 0;
        // The same change makes the process undumpable (suid_dumpable is 0):
        // a daemon that drops from root to a user still holds what it read
        // as root, and that user must not be able to read it back out of
        // its memory. For the whole process, as Linux's per-mm flag is.
        if (current->group != NULL)
            atomic_store(&current->group->undumpable, true);
    }
}

// CAP_SETUID and CAP_SETGID, over the caller's own user namespace: the ids
// it may then take are the ones that namespace maps.
static bool current_can_setuids(void) {
    return ns_capable(current_user_ns(), CAP_SETUID_);
}

static bool current_can_setgids(void) {
    return ns_capable(current_user_ns(), CAP_SETGID_);
}

static int put_id(guest_addr_t addr, uid_t_ id) {
    return user_put(addr, id);
}

// An id a set*id call was given, as a kernel id. -1 ("leave it") stays -1;
// false for one the caller's namespace does not map, which is EINVAL.
static bool arg_kuid(uid_t_ *uid) {
    if (*uid == (uid_t_) -1)
        return true;
    *uid = current_make_kuid(*uid);
    return *uid != USER_NS_INVALID_ID;
}
static bool arg_kgid(uid_t_ *gid) {
    if (*gid == (uid_t_) -1)
        return true;
    *gid = current_make_kgid(*gid);
    return *gid != USER_NS_INVALID_ID;
}

static void cap_emulate_setxuid(uid_t_ old_ruid, uid_t_ old_euid, uid_t_ old_suid) {
    // "Root" is the id that is 0 in the task's own namespace.
    const uid_t_ root = task_root_kuid(current);
    bool old_any_root = old_ruid == root || old_euid == root || old_suid == root;
    bool new_any_root = current->uid == root || current->euid == root || current->suid == root;

    // Linux drops all capabilities once a root-originating task has fully
    // transitioned to non-root credentials unless PR_SET_KEEPCAPS or
    // securebits say otherwise -- EXCEPT the ambient set, whose bits stay in
    // permitted and effective across the transition (that is the entire
    // point of ambient capabilities; systemd's AmbientCapabilities= relies
    // on it, and then re-asserts the bits with capset which our
    // permitted-subset check would reject if they had been cleared).
    if (old_any_root && !new_any_root) {
        current->cap_effective[0] = current->cap_ambient[0];
        current->cap_effective[1] = current->cap_ambient[1];
        if (!current->keepcaps) {
            current->cap_permitted[0] = current->cap_ambient[0];
            current->cap_permitted[1] = current->cap_ambient[1];
        } else {
            current->cap_permitted[0] |= current->cap_ambient[0];
            current->cap_permitted[1] |= current->cap_ambient[1];
        }
        return;
    }

    // Dropping only the effective uid from 0 disables effective capabilities
    // (minus ambient ones) until/unless the task returns to euid 0.
    if (old_euid == root && current->euid != root) {
        current->cap_effective[0] = current->cap_ambient[0];
        current->cap_effective[1] = current->cap_ambient[1];
        return;
    }

    // Regaining euid 0 restores effective capabilities from the permitted set.
    if (old_euid != root && current->euid == root) {
        current->cap_effective[0] = current->cap_permitted[0];
        current->cap_effective[1] = current->cap_permitted[1];
    }
}

pid_t_ sys_getpid(void) {
    STRACE("getpid()");
    return current_pid_shown(current->tgid);
}
pid_t_ sys_gettid(void) {
    STRACE("gettid()");
    return current_pid_shown(current->pid);
}
pid_t_ sys_getppid(void) {
    STRACE("getppid()");
    pid_t_ ppid;
    complex_lockt(&pids_lock, 0);
    // The parent PROCESS, i.e. the parent's tgid -- Linux returns
    // task_tgid_vnr(real_parent). `parent` points at the specific task that
    // forked us, which for a fork from a non-leader thread is NOT the group
    // leader, and `->pid` there is that thread's tid (the value sys_gettid
    // returns), not a process id at all.
    //
    // Getting this wrong is invisible while the forking task is its group's
    // leader, which is every shell and most single-threaded programs. It broke
    // Go outright: syscall.forkAndExecInChild, with SysProcAttr.Pdeathsig set,
    // captures getpid() in the parent and then has the child check
    // `getppid() != ppid` as "my parent already died" and kill itself with
    // Pdeathsig. Go runs the fork on whatever thread the runtime scheduled, so
    // on a non-leader the check misfired and the child SIGTERMed itself before
    // reaching execve -- surfacing only as os/exec's "signal: terminated", with
    // no error from the command, because the command never ran (GH #523, yay).
    //
    // And the parent of the PROCESS, asked from any thread: `current->parent`
    // of a thread is the thread that created it, in this same process, so
    // getppid() from every thread but the first returned this process's own
    // pid. Go asks from whichever thread the runtime is on.
    struct task *parent = task_process_parent(current);
    // 0 for a parent outside the caller's pid namespace: its init's is.
    ppid = parent != NULL && task_visible(parent) ? current_pid_shown(parent->tgid) : 0;
    unlock(&pids_lock);
    return ppid;
}

dword_t sys_getuid32(void) {
    STRACE("getuid32()");
    return current_uid_shown(current->uid);
}
dword_t sys_getuid(void) {
    STRACE("getuid()");
    return current_uid_shown(current->uid) & 0xffff;
}

dword_t sys_geteuid32(void) {
    STRACE("geteuid32()");
    return current_uid_shown(current->euid);
}
dword_t sys_geteuid(void) {
    STRACE("geteuid()");
    return current_uid_shown(current->euid) & 0xffff;
}

int_t sys_setuid(uid_t_ uid) {
    STRACE("setuid(%d)", uid);
    if (uid == (uid_t_) -1 || !arg_kuid(&uid))
        return _EINVAL;
    struct cred_change change;
    cred_change_begin(&change);
    uid_t_ old_ruid = current->uid;
    uid_t_ old_euid = current->euid;
    uid_t_ old_suid = current->suid;
    if (current_can_setuids()) {
        current->uid = current->suid = uid;
    } else {
        if (uid != current->uid && uid != current->suid)
            return _EPERM;
    }
    current->euid = uid;
    current->fsuid = uid;
    cap_emulate_setxuid(old_ruid, old_euid, old_suid);
    cred_change_commit(&change);
    return 0;
}

static dword_t setresuid_kernel(uid_t_ ruid, uid_t_ euid, uid_t_ suid);

dword_t sys_setresuid(uid_t_ ruid, uid_t_ euid, uid_t_ suid) {
    STRACE("setresuid(%d, %d, %d)", ruid, euid, suid);
    if (!arg_kuid(&ruid) || !arg_kuid(&euid) || !arg_kuid(&suid))
        return _EINVAL;
    return setresuid_kernel(ruid, euid, suid);
}

// The same with kernel ids, -1 for "leave it".
static dword_t setresuid_kernel(uid_t_ ruid, uid_t_ euid, uid_t_ suid) {
    struct cred_change change;
    cred_change_begin(&change);
    uid_t_ old_ruid = current->uid;
    uid_t_ old_euid = current->euid;
    uid_t_ old_suid = current->suid;
    if (!current_can_setuids()) {
        if (ruid != (uid_t) -1 && ruid != current->uid && ruid != current->euid && ruid != current->suid)
            return _EPERM;
        if (euid != (uid_t) -1 && euid != current->uid && euid != current->euid && euid != current->suid)
            return _EPERM;
        if (suid != (uid_t) -1 && suid != current->uid && suid != current->euid && suid != current->suid)
            return _EPERM;
    }

    if (ruid != (uid_t) -1)
        current->uid = ruid;
    if (euid != (uid_t) -1)
        current->euid = euid;
    if (suid != (uid_t) -1)
        current->suid = suid;
    if (euid != (uid_t) -1)
        current->fsuid = euid;
    cap_emulate_setxuid(old_ruid, old_euid, old_suid);
    cred_change_commit(&change);
    return 0;
}

int_t sys_getresuid(addr_t ruid_addr, addr_t euid_addr, addr_t suid_addr) {
    STRACE("getresuid(%#x, %#x, %#x)", ruid_addr, euid_addr, suid_addr);
    if (put_id(ruid_addr, current_uid_shown(current->uid)))
        return _EFAULT;
    if (put_id(euid_addr, current_uid_shown(current->euid)))
        return _EFAULT;
    if (put_id(suid_addr, current_uid_shown(current->suid)))
        return _EFAULT;
    return 0;
}

int_t sys_getresuid_guest(guest_addr_t ruid_addr, guest_addr_t euid_addr, guest_addr_t suid_addr) {
    STRACE("getresuid(%#llx, %#llx, %#llx)",
            (unsigned long long) ruid_addr,
            (unsigned long long) euid_addr,
            (unsigned long long) suid_addr);
    if (put_id(ruid_addr, current_uid_shown(current->uid)))
        return _EFAULT;
    if (put_id(euid_addr, current_uid_shown(current->euid)))
        return _EFAULT;
    if (put_id(suid_addr, current_uid_shown(current->suid)))
        return _EFAULT;
    return 0;
}

int_t sys_setreuid(uid_t_ ruid, uid_t_ euid) {
    // setreuid(2): "If the real user ID is set, or the effective user ID is
    // set to a value not equal to the previous real user ID, the saved
    // set-user-ID will be set to the new effective user ID."
    //
    // Passing -1 left the saved id at its OLD value, so a process that dropped
    // privilege with setreuid(1000, 1000) kept suid 0 and could call setuid(0)
    // to become root again. Making the drop permanent is the entire reason to
    // call setreuid, so this was worth more than its severity suggests.
    //
    // The computed saved value is always the NEW euid, which sys_setresuid's
    // own euid check has already validated, so passing it explicitly cannot
    // turn a permitted call into EPERM.
    if (!arg_kuid(&ruid) || !arg_kuid(&euid))
        return _EINVAL;
    uid_t_ new_euid = (euid == (uid_t_) -1) ? current->euid : euid;
    uid_t_ suid = (uid_t_) -1;
    if (ruid != (uid_t_) -1 || new_euid != current->uid)
        suid = new_euid;
    return setresuid_kernel(ruid, euid, suid);
}

uid_t_ sys_setfsuid(uid_t_ uid) {
    // The old id as the caller knows it; an id its namespace does not map
    // changes nothing, like one it may not take.
    uid_t_ old = current_uid_shown(current->fsuid);
    STRACE("setfsuid(%d)", uid);
    if (uid == (uid_t_) -1 || !arg_kuid(&uid))
        return old;
    if (current_can_setuids() || uid == current->uid || uid == current->euid || uid == current->suid) {
        struct cred_change change;
        cred_change_begin(&change);
        current->fsuid = uid;
        cred_change_commit(&change);
    }
    return old;
}

dword_t sys_getgid32(void) {
    STRACE("getgid32()");
    return current_gid_shown(current->gid);
}
dword_t sys_getgid(void) {
    STRACE("getgid()");
    return current_gid_shown(current->gid) & 0xffff;
}

dword_t sys_getegid32(void) {
    STRACE("getegid32()");
    return current_gid_shown(current->egid);
}
dword_t sys_getegid(void) {
    STRACE("getegid()");
    return current_gid_shown(current->egid) & 0xffff;
}

int_t sys_setgid(uid_t_ gid) {
    STRACE("setgid(%d)", gid);
    if (gid == (uid_t_) -1 || !arg_kgid(&gid))
        return _EINVAL;
    struct cred_change change;
    cred_change_begin(&change);
    if (current_can_setgids()) {
        current->gid = current->sgid = gid;
    } else {
        if (gid != current->gid && gid != current->sgid)
            return _EPERM;
    }
    current->egid = gid;
    current->fsgid = gid;
    cred_change_commit(&change);
    return 0;
}

static dword_t setresgid_kernel(uid_t_ rgid, uid_t_ egid, uid_t_ sgid);

dword_t sys_setresgid(uid_t_ rgid, uid_t_ egid, uid_t_ sgid) {
    STRACE("setresgid(%d, %d, %d)", rgid, egid, sgid);
    if (!arg_kgid(&rgid) || !arg_kgid(&egid) || !arg_kgid(&sgid))
        return _EINVAL;
    return setresgid_kernel(rgid, egid, sgid);
}

static dword_t setresgid_kernel(uid_t_ rgid, uid_t_ egid, uid_t_ sgid) {
    struct cred_change change;
    cred_change_begin(&change);
    if (!current_can_setgids()) {
        if (rgid != (uid_t) -1 && rgid != current->gid && rgid != current->egid && rgid != current->sgid)
            return _EPERM;
        if (egid != (uid_t) -1 && egid != current->gid && egid != current->egid && egid != current->sgid)
            return _EPERM;
        if (sgid != (uid_t) -1 && sgid != current->gid && sgid != current->egid && sgid != current->sgid)
            return _EPERM;
    }

    if (rgid != (uid_t) -1)
        current->gid = rgid;
    if (egid != (uid_t) -1)
        current->egid = egid;
    if (sgid != (uid_t) -1)
        current->sgid = sgid;
    if (egid != (uid_t) -1)
        current->fsgid = egid;
    cred_change_commit(&change);
    return 0;
}

int_t sys_getresgid(addr_t rgid_addr, addr_t egid_addr, addr_t sgid_addr) {
    STRACE("getresgid(%#x, %#x, %#x)", rgid_addr, egid_addr, sgid_addr);
    if (put_id(rgid_addr, current_gid_shown(current->gid)))
        return _EFAULT;
    if (put_id(egid_addr, current_gid_shown(current->egid)))
        return _EFAULT;
    if (put_id(sgid_addr, current_gid_shown(current->sgid)))
        return _EFAULT;
    return 0;
}

int_t sys_getresgid_guest(guest_addr_t rgid_addr, guest_addr_t egid_addr, guest_addr_t sgid_addr) {
    STRACE("getresgid(%#llx, %#llx, %#llx)",
            (unsigned long long) rgid_addr,
            (unsigned long long) egid_addr,
            (unsigned long long) sgid_addr);
    if (put_id(rgid_addr, current_gid_shown(current->gid)))
        return _EFAULT;
    if (put_id(egid_addr, current_gid_shown(current->egid)))
        return _EFAULT;
    if (put_id(sgid_addr, current_gid_shown(current->sgid)))
        return _EFAULT;
    return 0;
}

int_t sys_setregid(uid_t_ rgid, uid_t_ egid) {
    // The gid half of the same rule; see sys_setreuid.
    if (!arg_kgid(&rgid) || !arg_kgid(&egid))
        return _EINVAL;
    uid_t_ new_egid = (egid == (uid_t_) -1) ? current->egid : egid;
    uid_t_ sgid = (uid_t_) -1;
    if (rgid != (uid_t_) -1 || new_egid != current->gid)
        sgid = new_egid;
    return setresgid_kernel(rgid, egid, sgid);
}

uid_t_ sys_setfsgid(uid_t_ gid) {
    uid_t_ old = current_gid_shown(current->fsgid);
    STRACE("setfsgid(%d)", gid);
    if (gid == (uid_t_) -1 || !arg_kgid(&gid))
        return old;
    if (current_can_setgids() || gid == current->gid || gid == current->egid || gid == current->sgid) {
        struct cred_change change;
        cred_change_begin(&change);
        current->fsgid = gid;
        cred_change_commit(&change);
    }
    return old;
}

int_t sys_getgroups(dword_t size, addr_t list) {
    return sys_getgroups_guest(size, list);
}

int_t sys_getgroups_guest(dword_t size, guest_addr_t list) {
    STRACE("getgroups(%d, %#x)", size, list);
    if (size == 0)
        return current->ngroups;
    if (size < current->ngroups)
        return _EINVAL;
    for (unsigned i = 0; i < current->ngroups; i++)
        STRACE(" %d", current->groups[i]);
    if (current_user_ns() == NULL) {
        if (user_write(list, current->groups, current->ngroups * sizeof(uid_t_)))
            return _EFAULT;
        return current->ngroups;
    }
    // As the caller's namespace names them: a group it does not map is the
    // overflow group.
    for (unsigned i = 0; i < current->ngroups; i++) {
        uid_t_ shown = current_gid_shown(current->groups[i]);
        if (user_write(list + i * sizeof(uid_t_), &shown, sizeof(shown)))
            return _EFAULT;
    }
    return current->ngroups;
}

int_t sys_setgroups(dword_t size, addr_t list) {
    return sys_setgroups_guest(size, list);
}

int_t sys_setgroups_guest(dword_t size, guest_addr_t list) {
    STRACE("setgroups(%d, %#x)", size, list);
    // In a user namespace, only once its gid map is written and setgroups
    // has not been denied there (/proc/<pid>/setgroups): dropping a group
    // is otherwise a way past a file's group-deny bits.
    if (!current_can_setgids() || !user_ns_may_setgroups(current_user_ns()))
        return _EPERM;
    if (size > MAX_GROUPS)
        return _EINVAL;
    // Built to the side and swapped in, so a faulting list leaves the current
    // group set intact rather than half-overwritten.
    uid_t_ *groups = NULL;
    if (size != 0) {
        groups = malloc((size_t) size * sizeof(uid_t_));
        if (groups == NULL)
            return _ENOMEM;
        if (user_read(list, groups, (size_t) size * sizeof(uid_t_))) {
            free(groups);
            return _EFAULT;
        }
    }
    for (unsigned i = 0; i < size; i++) {
        STRACE(" %d", groups[i]);
        groups[i] = current_make_kgid(groups[i]);
        if (groups[i] == USER_NS_INVALID_ID) {
            free(groups);
            return _EINVAL;
        }
    }
    free(current->groups);
    current->groups = groups;
    current->ngroups = size;
    return 0;
}

// this does not really work
int_t sys_capget(addr_t header_addr, addr_t data_addr) {
    return sys_capget_guest(header_addr, data_addr);
}

int_t sys_capget_guest(guest_addr_t header_addr, guest_addr_t data_addr) {
    STRACE("capget(%#llx, %#llx)", (unsigned long long) header_addr, (unsigned long long) data_addr);
    struct cap_user_header_ header;
    if (user_read(header_addr, &header, sizeof(header)))
        return _EFAULT;
    int count = cap_data_count(header.version);
    if (count < 0) {
        header.version = _LINUX_CAPABILITY_VERSION_3_;
        if (user_write(header_addr, &header, sizeof(header)))
            return _EFAULT;
        return _EINVAL;
    }
    // Accept the tgid too: glibc/systemd pass getpid() (the tgid) here,
    // while current->pid is the tid.
    if (header.pid != 0 && current_pid_global(header.pid) != current->pid && current_pid_global(header.pid) != current->tgid)
        return _EPERM;

    struct cap_user_data_ data[2] = {};
    data[0].effective = current->cap_effective[0];
    data[0].permitted = current->cap_permitted[0];
    data[0].inheritable = current->cap_inheritable[0];
    if (count > 1) {
        data[1].effective = current->cap_effective[1];
        data[1].permitted = current->cap_permitted[1];
        data[1].inheritable = current->cap_inheritable[1];
    }
    if (user_write(data_addr, data, sizeof(data[0]) * count))
        return _EFAULT;
    return 0;
}
int_t sys_capset(addr_t header_addr, addr_t data_addr) {
    return sys_capset_guest(header_addr, data_addr);
}

int_t sys_capset_guest(guest_addr_t header_addr, guest_addr_t data_addr) {
    STRACE("capset(%#llx, %#llx)", (unsigned long long) header_addr, (unsigned long long) data_addr);
    struct cap_user_header_ header;
    if (user_read(header_addr, &header, sizeof(header)))
        return _EFAULT;
    int count = cap_data_count(header.version);
    if (count < 0)
        return _EINVAL;
    if (header.pid != 0 && current_pid_global(header.pid) != current->pid && current_pid_global(header.pid) != current->tgid)
        return _EPERM;

    struct cap_user_data_ data[2] = {};
    if (user_read(data_addr, data, sizeof(data[0]) * count))
        return _EFAULT;

    dword_t new_effective[2] = {data[0].effective, 0};
    dword_t new_permitted[2] = {data[0].permitted, 0};
    dword_t new_inheritable[2] = {data[0].inheritable, 0};
    if (count > 1) {
        new_effective[1] = data[1].effective;
        new_permitted[1] = data[1].permitted;
        new_inheritable[1] = data[1].inheritable;
    }

    // Linux allows an unprivileged task to drop capabilities it already has
    // and to toggle effective bits within its permitted set. The old
    // superuser-only gate breaks helpers like ping that reduce their
    // capability set after a uid transition.
    if (!superuser()) {
        if (!cap_words_subset(new_permitted, current->cap_permitted, count))
            return _EPERM;
        // Linux permits raising inheritable bits from the PERMITTED set too
        // (pI' must be a subset of pI | pP, not of pI alone): systemd's
        // keep_capability() after enforce_user's setresuid re-asserts a cap
        // in all three of effective/permitted/inheritable, and the cap is
        // in permitted (kept across the uid switch via SECBIT_KEEP_CAPS)
        // but typically NOT in the pre-existing inheritable set.
        dword_t inheritable_limit[2] = {
            current->cap_inheritable[0] | current->cap_permitted[0],
            current->cap_inheritable[1] | current->cap_permitted[1],
        };
        if (!cap_words_subset(new_inheritable, inheritable_limit, count))
            return _EPERM;
        if (!cap_words_subset(new_effective, new_permitted, count))
            return _EPERM;
        if (!cap_words_subset(new_effective, current->cap_permitted, count))
            return _EPERM;
    }

    if (!cap_words_subset(new_effective, new_permitted, count))
        return _EPERM;
    // Nobody -- root included -- puts a capability back into the inheritable
    // set once the bounding set has dropped it (Linux's "no new pI
    // capabilities outside bounding set").
    dword_t inheritable_ceiling[2] = {
        current->cap_inheritable[0] | current->cap_bounding[0],
        current->cap_inheritable[1] | current->cap_bounding[1],
    };
    if (!cap_words_subset(new_inheritable, inheritable_ceiling, count))
        return _EPERM;

    struct cred_change change;
    cred_change_begin(&change);
    current->cap_effective[0] = new_effective[0];
    current->cap_permitted[0] = new_permitted[0];
    current->cap_inheritable[0] = new_inheritable[0];
    current->cap_effective[1] = 0;
    current->cap_permitted[1] = 0;
    current->cap_inheritable[1] = 0;
    if (count > 1) {
        current->cap_effective[1] = new_effective[1];
        current->cap_permitted[1] = new_permitted[1];
        current->cap_inheritable[1] = new_inheritable[1];
    }
    // The ambient set only ever holds what is both permitted and inheritable,
    // so a capability dropped from either leaves it too (Linux's cap_capset).
    for (int i = 0; i < 2; i++)
        current->cap_ambient[i] &= current->cap_permitted[i] & current->cap_inheritable[i];
    cred_change_commit(&change);
    return 0;
}

// minimal version according to Linux sys/personality.h
// personality() sets the execution domain and returns the PREVIOUS value.
// Linux accepts whatever it is given -- the low byte is the domain, the upper
// bits are flags -- and 0xffffffff is the conventional "just tell me" query
// because it sets nothing anybody uses.
//
// This used to refuse everything except ADDR_NO_RANDOMIZE with EINVAL,
// including personality(0), which is the single most common call: it is what
// `setarch --uname-2.6`, ancient-binary wrappers and every "restore the
// default domain" path do. They all failed.
int_t sys_personality(dword_t persona) {
    STRACE("personality(%#x)", persona);
    dword_t previous = current->group->personality;
    if (persona == 0xffffffff)
        return previous;

    // Stored as given and reported back. ADDR_NO_RANDOMIZE used to be forced
    // on, because the address space was not randomized and a bit saying it
    // was would have been a lie; exec randomizes the layout now
    // (kernel/exec.c), and the bit is what turns that off -- setarch -R,
    // and gdb's disable-randomization, which is on by default.
    current->group->personality = persona;
    return previous;
}
