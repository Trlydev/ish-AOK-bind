// ish-AOK-bind: this fork's /proc/ish entries (fs/proc/ish_fork.c and the
// drivers' own show/update functions), spliced into upstream's table in
// fs/proc/ish.c by its first line. Lookup there is a linear scan by name, so
// where they sit in the table does not matter.
#ifndef FS_PROC_ISH_FORK_H
#define FS_PROC_ISH_FORK_H

#include <sys/stat.h>
#include "fs/proc.h"
#include "kernel/binder.h"
#include "kernel/property_area.h"
#include "kernel/logd_sink.h"

int proc_ish_show_arm64_faultdump(struct proc_entry *entry, struct proc_data *buf);
int proc_ish_update_arm64_faultdump(struct proc_entry *entry, struct proc_data *data);
int proc_ish_show_arm64_watch(struct proc_entry *entry, struct proc_data *buf);
int proc_ish_update_arm64_watch(struct proc_entry *entry, struct proc_data *data);
int proc_ish_show_cpu_features(struct proc_entry *entry, struct proc_data *buf);

#define FORK_PROC_ISH_ENTRIES \
    {"arm64_watch", S_IFREG | 0644, .show = proc_ish_show_arm64_watch, .update = proc_ish_update_arm64_watch}, \
    {"arm64_faultdump", S_IFREG | 0644, .show = proc_ish_show_arm64_faultdump, .update = proc_ish_update_arm64_faultdump}, \
    {"binder", .show = binder_show_state}, \
    {"cpu_features", .show = proc_ish_show_cpu_features}, \
    {"logd", S_IFREG | 0644, .show = logd_sink_show, .update = logd_sink_update}, \
    {"property_area", S_IFREG | 0644, .show = property_area_show, .update = property_area_update},

#endif
