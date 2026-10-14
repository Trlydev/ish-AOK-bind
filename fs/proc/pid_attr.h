// ish-AOK-bind: /proc/<pid>/attr/ (fs/proc/pid_attr.c).
#ifndef FS_PROC_PID_ATTR_H
#define FS_PROC_PID_ATTR_H
#include <stdbool.h>
struct proc_entry;
bool proc_pid_attr_readdir(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry);
void proc_pid_attr_init(void);
#endif
