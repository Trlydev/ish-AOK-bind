#ifndef KERNEL_PID_NS_H
#define KERNEL_PID_NS_H

#include <stdatomic.h>
#include <stdbool.h>
#include "misc.h"

struct task;
struct user_namespace;

// A pid namespace (CLONE_NEWPID): a set of processes that can see only each
// other, numbered from 1.
//
// AOK keeps one pid table, and a task has one slot in it: its MACHINE pid,
// which is what the kernel holds and compares everywhere. A namespace gives
// the tasks in it -- its own and those of namespaces made inside it -- a
// second number each, handed out from 1 as Linux hands them out, so its first
// process is pid 1, the next 2. A task three namespaces deep has four
// numbers: the machine's and one in each namespace above it. Two things
// follow, both asked of the task that is looking (`current`):
//
//   - a number names a task only in the namespace it was given in. From
//     inside, the lookups by number (pid_get, pid_get_task, ... in
//     kernel/task.h) read a number as the caller's namespace's and find only
//     tasks in that namespace or below it. Every other pid is not there:
//     ESRCH to kill, absent from /proc, 0 as a parent.
//   - a number given back to a guest is the one its namespace has for the
//     task (current_pid_shown), and 0 for a task it has no number for; one a
//     guest gives that is kept for later -- a process group, a terminal's
//     foreground group -- is kept as the machine's (current_pid_global).
//
// So a number a guest hands the kernel is confined without the call site's
// knowing, and what holds a machine number instead -- a stored tgid, a
// timer's target, the app -- asks pid_get_*_global.
//
// The init is the namespace: when it exits every process left inside is
// killed, and nothing more can be forked into it. Orphans inside go to it. A
// signal sent to it from inside does only what it has a handler for.
//
// The initial namespace is `task->pid_ns == NULL`, and its numbers are the
// machine's.
struct pid_namespace {
    atomic_uint refcount;
    unsigned long inode;            // its identity in /proc/<pid>/ns/pid
    struct pid_namespace *parent;   // NULL: its parent is the initial one. Owns a reference.
    unsigned level;                 // 1 for a child of the initial namespace
    struct user_namespace *user_ns; // the user namespace it was made in. Owns a reference.
    // The machine number of its init: 0 until its first process is forked
    // into it (unshare makes a namespace nobody is in yet).
    _Atomic pid_t_ init_pid;
    // Its init has exited.
    atomic_bool dead;
    // Its numbers, both ways, each MAX_PID + 1 long and indexed by a pid:
    // nr_of[machine pid] is that task's number here (0: it has none), and
    // by_nr[number] the machine pid it is. Written under pids_lock; read
    // without it, an int at a time, by whoever holds the namespace.
    pid_t_ *nr_of, *by_nr;
    pid_t_ last_nr;                 // the last one handed out; guarded by pids_lock
};

#define PID_NS_MAX_LEVEL 32
// What a number that names nothing in a namespace becomes: past the table.
#define PID_NS_NO_PID 0x7fffffff

// NULL-is-initial pointers throughout: retain and release accept NULL.
struct pid_namespace *pid_ns_retain(struct pid_namespace *ns);
void pid_ns_release(struct pid_namespace *ns);
// A new, empty namespace below `parent`, owned by `owner`; NULL when out of
// memory or past 32 levels (*err says which).
struct pid_namespace *pid_ns_new(struct pid_namespace *parent, struct user_namespace *owner, int *err);
// Is `ns` the namespace `ancestor`, or inside it?
bool pid_ns_in(const struct pid_namespace *ns, const struct pid_namespace *ancestor);

// The calling task's namespace, NULL for the initial one (and off a task).
struct pid_namespace *current_pid_ns(void);
// A machine number as tasks in `viewer` know it -- 0 for a task that has no
// number there -- and the reverse (PID_NS_NO_PID for a number that names
// nothing there). 0 and negative numbers are left alone: they are not pids.
pid_t_ pid_ns_shown(struct pid_namespace *viewer, pid_t_ pid);
pid_t_ pid_ns_global(struct pid_namespace *viewer, pid_t_ pid);
pid_t_ current_pid_shown(pid_t_ pid);
pid_t_ current_pid_global(pid_t_ pid);
// The same for a number that may name a task the caller cannot see: 0 then,
// as Linux reports a parent or a sender outside the namespace. Takes
// pids_lock.
pid_t_ current_pid_shown_or_zero(pid_t_ pid);
// `pid` as `task` itself knows it: what it was told its own or a relative's
// pid is.
pid_t_ task_pid_shown(struct task *task, pid_t_ pid);
// May the calling task see `task`? (Caller holds pids_lock or a reference.)
bool task_visible(struct task *task);
// Is `task` the init process of a namespace the calling task is in (its own
// namespace's, or of one above it that... no: only of its own or one it is
// inside)? What a signal from the calling task to it is limited by.
bool task_is_init_seen_from_inside(struct task *task);

#endif
