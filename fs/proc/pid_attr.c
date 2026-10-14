// ish-AOK-bind: /proc/<pid>/attr/, the SELinux process attributes. Kept out
// of fs/proc/pid.c, which upstream edits constantly, so a sync merges around
// two one-line hooks there (the "attr" table entry and proc_pid_attr_init).
// See docs/fork-maintenance.md.

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "fs/proc.h"
#include "fs/proc/pid_attr.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/task.h"

// fs/proc/pid.c's proc_get_task/proc_put_task, which are static there:
// zombies included, and a reference the caller drops.
static struct task *attr_get_task(struct proc_entry *entry) {
    return pid_get_task_zombie_global_ref(entry->pid);
}
static void attr_put_task(struct task *task) {
    if (task != NULL)
        task_ref_cnt_mod(task, -1);
}

// /proc/<pid>/attr/ -- the SELinux process attributes.
//
// libselinux is built entirely on these files: getcon/setcon are a read/write
// of "current", getprevcon reads "prev", and setexeccon stages "exec" for the
// next execve (kernel/exec.c consumes it). None of it confines anything -- see
// fs/selinuxfs.c for why the whole SELinux surface here is a permissive stub --
// but the files have to exist, because Android userspace treats their absence
// as fatal rather than as "SELinux is off": servicemanager's first act is
// CHECK(getcon(&mThisProcessContext) == 0), and getcon() on a missing file
// returns -1.
//
// The contexts live in struct task (kernel/task.h), so a thread's attributes
// are its own and fork inherits them, matching Linux.
struct proc_attr_type {
    const char *name;
    size_t offset; // into struct task_security
    // "current" is what getcon() returns and must always name something; the
    // staging slots are meaningfully empty, and libselinux clears them by
    // writing nothing (setexeccon(NULL) and friends).
    bool clearable;
};

static const struct proc_attr_type proc_attr_types[] = {
    {"current",   offsetof(struct task_security, current),   false},
    {"exec",      offsetof(struct task_security, exec),      true},
    {"fscreate",  offsetof(struct task_security, fscreate),  true},
    {"keycreate", offsetof(struct task_security, keycreate), true},
    {"prev",      offsetof(struct task_security, prev),      true},
    {"sockcreate", offsetof(struct task_security, sockcreate), true},
};
#define PROC_ATTR_TYPES_LEN (sizeof(proc_attr_types) / sizeof(proc_attr_types[0]))

static struct proc_dir_entry proc_pid_attr_entry;

static char *proc_attr_slot(struct task *task, const struct proc_attr_type *type) {
    return (char *) &task->security + type->offset;
}

bool proc_pid_attr_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    struct task *task = attr_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        attr_put_task(task);
        return false;
    }
    attr_put_task(task);
    if (*index >= PROC_ATTR_TYPES_LEN)
        return false;
    *next_entry = (struct proc_entry) {&proc_pid_attr_entry, .pid = entry->pid, .fd = (sdword_t) *index};
    (*index)++;
    return true;
}

static void proc_pid_attr_getname(struct proc_entry *entry, char *buf) {
    snprintf(buf, 256, "%s", proc_attr_types[entry->fd].name);
}

static int proc_pid_attr_show(struct proc_entry *entry, struct proc_data *buf) {
    struct task *task = attr_get_task(entry);
    if ((task == NULL) || (task->exiting == true)) {
        attr_put_task(task);
        return _ESRCH;
    }
    const struct proc_attr_type *type = &proc_attr_types[entry->fd];
    char context[TASK_SECURITY_CONTEXT_MAX];
    lock(&task->general_lock, 0);
    strcpy(context, proc_attr_slot(task, type));
    unlock(&task->general_lock);
    attr_put_task(task);

    // An unset attribute reads as zero bytes, which is how libselinux
    // distinguishes "no context staged" from a context it should use.
    if (context[0] == '\0') {
        // Except that "current" is never unset. A task created without a
        // parent to inherit from would otherwise hand getcon() a NULL context
        // and a success return, and the caller would dereference it.
        if (type->clearable)
            return 0;
        strcpy(context, TASK_SECURITY_DEFAULT_CONTEXT);
    }
    // Linux includes the terminating NUL in the length it reports, and
    // appends no newline. libselinux does not depend on the NUL, but tools
    // that echo the file do depend on there being no newline to confuse with
    // part of the context.
    proc_buf_append(buf, context, strlen(context) + 1);
    return 0;
}

static int proc_pid_attr_update(struct proc_entry *entry, struct proc_data *data) {
    // Linux allows only the calling task itself, and means the task, not the
    // thread group: writing /proc/<tgid>/attr/current from a non-leader thread
    // is EACCES there rather than a relabel of the leader. These attributes are
    // per-thread, so targeting anything but the caller is always a mistake.
    if (entry->pid != current->pid)
        return _EACCES;

    const struct proc_attr_type *type = &proc_attr_types[entry->fd];

    // libselinux writes the context with its trailing NUL included, and some
    // callers add a newline; neither is part of the context.
    size_t len = data->size;
    while (len > 0 && (data->data[len - 1] == '\0' || data->data[len - 1] == '\n'))
        len--;

    if (len == 0) {
        if (!type->clearable)
            return _EINVAL;
        struct task *task = attr_get_task(entry);
        if (task == NULL)
            return _ESRCH;
        lock(&task->general_lock, 0);
        proc_attr_slot(task, type)[0] = '\0';
        unlock(&task->general_lock);
        attr_put_task(task);
        return 0;
    }

    if (len >= TASK_SECURITY_CONTEXT_MAX)
        return _EINVAL;
    if (memchr(data->data, '\0', len) != NULL)
        return _EINVAL;
    // Reject anything that is not user:role:type:range. There is no policy to
    // validate against, but accepting a malformed context would push the
    // failure into whatever later parses it, far from the write that caused it.
    int colons = 0;
    for (size_t i = 0; i < len; i++)
        if (data->data[i] == ':')
            colons++;
    if (colons < 3)
        return _EINVAL;

    struct task *task = attr_get_task(entry);
    if (task == NULL)
        return _ESRCH;
    lock(&task->general_lock, 0);
    char *slot = proc_attr_slot(task, type);
    memcpy(slot, data->data, len);
    slot[len] = '\0';
    unlock(&task->general_lock);
    attr_put_task(task);
    return 0;
}

// 0666 like Linux: setcon() opens it for writing, and the caller is usually not
// root by the time it relabels itself.
static struct proc_dir_entry proc_pid_attr_entry = {NULL, S_IFREG | 0666,
    .getname = proc_pid_attr_getname, .show = proc_pid_attr_show,
    // An empty write to an attr file is the CLEAR operation, and clearing a
    // non-clearable slot must report EINVAL -- both live in the handler's
    // len == 0 branch, so it has to actually be reached. Without this,
    // fs/proc.c answers 0 and never calls update.
    .wants_empty_write = true,
    .update = proc_pid_attr_update};

extern struct proc_children proc_pid_children; // fs/proc/pid.c

void proc_pid_attr_init(void) {
    struct proc_dir_entry *attr_dir = proc_children_find(&proc_pid_children, "attr");
    if (attr_dir != NULL)
        proc_pid_attr_entry.parent = attr_dir;
}
