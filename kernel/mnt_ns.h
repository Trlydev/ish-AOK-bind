#ifndef KERNEL_MNT_NS_H
#define KERNEL_MNT_NS_H

#include <stdbool.h>
#include <stddef.h>

struct fs_info;

// A mount namespace: a mount table of a process's own (CLONE_NEWNS).
//
// AOK keeps every mount in one table keyed by its point, a path string, and a
// descriptor's path is its mount's point plus the path inside it (kernel/fs.h).
// A namespace is a region of that one table: its tree hangs under a root of its
// own, a staging point ("/.ish-fsmount/<n>", the same kind of place a lazily
// unmounted tree is parked at), and the tasks in it have their root there.
// Everything a staging point already means then holds for it -- no walk from
// outside can spell its way in, `..` at it stays at it, and a path is shown
// from it -- which is what a process sees of the root of its mount namespace.
//
// A new namespace starts as a copy of its parent's tree: each mount in the
// parent appears in it as a bind of the same filesystem, at the same place,
// with the same flags. Mounting, unmounting and re-pointing in one namespace
// then touch only that namespace's region. The initial namespace is the table
// as it has always been (root ""), and nothing about it changes.
//
// Nothing propagates between namespaces: every tree is private, the state
// mount --make-rprivate gives on Linux. A mount made in the parent after the
// copy does not appear in the child.
struct mnt_namespace {
    unsigned refcount;   // guarded by mounts_lock; tasks, and /proc ns descriptors
    unsigned long inode; // its identity in /proc/<pid>/ns/mnt. Immutable.
    char root[48];       // "" for the initial namespace. Immutable.
    size_t root_len;
};

// The namespace every task starts in. A task's mnt_ns is NULL for it.
extern struct mnt_namespace init_mnt_ns;

// Never NULL: the initial namespace for NULL.
static inline struct mnt_namespace *mnt_ns_or_init(struct mnt_namespace *ns) {
    return ns != NULL ? ns : &init_mnt_ns;
}
// The calling task's, or the initial one off a task thread.
struct mnt_namespace *mnt_ns_current(void);

// Both take NULL and the initial namespace, which is never freed, as no-ops.
struct mnt_namespace *mnt_ns_retain(struct mnt_namespace *ns);
void mnt_ns_release(struct mnt_namespace *ns);

// A new namespace holding a copy of `from`'s tree, with one reference; NULL
// when out of memory.
struct mnt_namespace *mnt_ns_copy(struct mnt_namespace *from);

// Move `fs`'s root and working directory from where they are in `from` to
// the same directories in `to` (for a copy), or to `to`'s root (setns, where
// the two trees need have nothing in common: `same_place` false). A working
// directory with no counterpart is left where it is, as a descriptor would be.
int mnt_ns_move_fs(struct fs_info *fs, struct mnt_namespace *from, struct mnt_namespace *to, bool same_place);

// pivot_root(2) for the calling task: `new_root` and `put_old` are normalized
// real-root paths (path_normalize's output).
int mnt_ns_pivot_root(const char *new_root, const char *put_old);

// Is `point` (a normalized real-root path) inside `ns`'s tree?
bool mnt_ns_has_point(const struct mnt_namespace *ns, const char *point);

#endif
