#ifndef KERNEL_USER_NS_H
#define KERNEL_USER_NS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include "misc.h"
#include "util/sync.h"

struct task;
struct statbuf;
struct proc_data;

// A user namespace (CLONE_NEWUSER): a view of the user and group ids, and a
// place to hold capabilities that are worth something only inside it.
//
// The ids a task carries (struct task's uid, euid, ... and its groups), the
// ids on files, and every id the kernel compares are KERNEL ids: the machine's
// own, the same in every namespace. A namespace has two maps from the ids its
// tasks use to kernel ids. What a task is told (getuid, stat, a signal's
// sender) is a kernel id read back through its namespace's map, and 65534 --
// the overflow id -- for one the map does not cover; what it asks for
// (setuid, chown) is an id of its namespace turned into a kernel id, and
// EINVAL for one with no mapping. A namespace's maps are written once, through
// /proc/<pid>/uid_map and gid_map, and hold kernel ids already resolved
// through the parent's.
//
// Privilege. A task that makes a user namespace has every capability IN it and
// none anywhere else, whatever its uid: superuser() and current_capable() are
// false for any task outside the initial namespace, so nothing the machine
// guards with them is reachable from inside one. What a capability held in a
// namespace does reach is asked with ns_capable(), naming the namespace that
// owns the thing being acted on:
//
//   - the other namespaces the task makes afterwards (mount, UTS, IPC), each
//     of which records the user namespace it was made in: mounting in a mount
//     namespace it owns, naming a UTS namespace it owns;
//   - ids: setuid/setgid to any id the namespace maps, setgroups where the
//     namespace allows it;
//   - files whose owner AND group the namespace maps (capable_wrt_inode):
//     chown among mapped ids, and the owner's and DAC overrides.
//
// Everything else a capability would grant on Linux inside a namespace is
// simply refused here, which is the safe direction to be wrong in.
//
// The initial namespace is `task->user_ns == NULL`, as with mount namespaces:
// its maps are the identity and its rules are the ones AOK has always had.

#define USER_NS_OVERFLOW_ID 65534u
#define USER_NS_INVALID_ID ((uid_t_) -1)
#define USER_NS_MAX_LEVEL 32
// Linux takes 340 lines in a map. Nothing writes more than a handful.
#define USER_NS_MAP_EXTENTS 340

struct id_extent {
    uid_t_ first;   // in the namespace
    uid_t_ lower;   // the kernel id `first` is
    uid_t_ count;
};

struct id_map {
    // Written once, under the namespace's lock, and `set` last: a reader that
    // sees it set sees the whole map.
    atomic_bool set;
    unsigned count;
    struct id_extent extent[USER_NS_MAP_EXTENTS];
};

struct user_namespace {
    atomic_uint refcount;
    unsigned long inode;            // its identity in /proc/<pid>/ns/user
    struct user_namespace *parent;  // NULL: its parent is the initial one. Owns a reference.
    unsigned level;                 // 1 for a child of the initial namespace
    uid_t_ owner, group;            // kernel ids: its creator's euid and egid
    lock_t lock;
    struct id_map uid_map, gid_map;
    bool setgroups_denied;          // /proc/<pid>/setgroups; guarded by lock
    // RLIMIT_NPROC for everything made in here, counted against its owner
    // one level up: the creator's soft limit when it made the namespace, or
    // none for root of the initial one (Linux's ucount_max[UCOUNT_RLIMIT_NPROC],
    // enforced_nproc_rlimit). See task_nproc_exceeded.
    qword_t nproc_max;              // (an rlim_t_)
};

// NULL-is-initial pointers throughout: retain and release accept NULL.
struct user_namespace *user_ns_retain(struct user_namespace *ns);
void user_ns_release(struct user_namespace *ns);
// A new namespace, a child of the calling task's, owned by its euid/egid; or
// an ERR_PTR: EPERM for a caller whose ids its own namespace does not map or
// that is in a chroot, ENOSPC past 32 levels.
struct user_namespace *user_ns_new(void);
// What a task that has just entered a namespace of its own holds: every
// capability, in that namespace.
void user_ns_enter(struct task *task, struct user_namespace *ns);
// Is `ns` the namespace `ancestor`, or inside it?
bool user_ns_in(const struct user_namespace *ns, const struct user_namespace *ancestor);

// An id of `ns` as a kernel id, or USER_NS_INVALID_ID. (uid_t)-1 itself is
// never valid: it is what "leave this one alone" is spelled as.
uid_t_ user_ns_make_kuid(struct user_namespace *ns, uid_t_ uid);
uid_t_ user_ns_make_kgid(struct user_namespace *ns, uid_t_ gid);
// A kernel id as `ns` sees it; USER_NS_INVALID_ID when unmapped, and from the
// _munged forms the overflow id instead, which is what a task is shown.
uid_t_ user_ns_from_kuid(struct user_namespace *ns, uid_t_ kuid);
uid_t_ user_ns_from_kgid(struct user_namespace *ns, uid_t_ kgid);
uid_t_ user_ns_from_kuid_munged(struct user_namespace *ns, uid_t_ kuid);
uid_t_ user_ns_from_kgid_munged(struct user_namespace *ns, uid_t_ kgid);

// The calling task's namespace, NULL for the initial one.
struct user_namespace *current_user_ns(void);
// The same four, for the calling task.
uid_t_ current_make_kuid(uid_t_ uid);
uid_t_ current_make_kgid(uid_t_ gid);
uid_t_ current_uid_shown(uid_t_ kuid);
uid_t_ current_gid_shown(uid_t_ kgid);
// The kernel id that is uid 0 to `task`, or USER_NS_INVALID_ID: the id whose
// coming and going moves capabilities (exec's root rule, setuid's).
uid_t_ task_root_kuid(struct task *task);

// Does the calling task hold `cap` over things owned by `target`? In the
// initial namespace for the initial namespace this is current_capable(). A
// task holds every capability over a namespace its euid made in its own, and
// what its effective set says over its own namespace.
bool ns_capable(struct user_namespace *target, unsigned cap);
// ...over a file with this owner and group: its namespace must map both.
bool capable_wrt_inode(const struct statbuf *stat, unsigned cap);
bool capable_wrt_ids(uid_t_ kuid, uid_t_ kgid, unsigned cap);

// setgroups(2): the namespace's gid map is written and setgroups not denied.
bool user_ns_may_setgroups(struct user_namespace *ns);

// /proc/<pid>/uid_map, gid_map and setgroups, for the namespace of the task
// the file belongs to. Writes are the calling task's.
void user_ns_show_map(struct user_namespace *ns, bool gid, struct proc_data *buf);
int user_ns_write_map(struct user_namespace *ns, bool gid, const char *text, size_t size);
void user_ns_show_setgroups(struct user_namespace *ns, struct proc_data *buf);
int user_ns_write_setgroups(struct user_namespace *ns, const char *text, size_t size);

#endif
