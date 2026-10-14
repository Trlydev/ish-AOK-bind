// ish-AOK-bind: this fork's /proc/ish entries. Upstream's table in
// fs/proc/ish.c takes all of them through one line (FORK_PROC_ISH_ENTRIES,
// fs/proc/ish_fork.h), so a sync never has to merge this fork's entries in
// among upstream's. See docs/fork-maintenance.md.

#include <stdio.h>
#include <string.h>
#include "fs/proc.h"
#include "fs/proc/ish_fork.h"
#include "emu/tlb.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/hostinfo.h"

// Which registers an arm64 page fault dumps memory around. Written as a comma
// separated list of register numbers ("19,20"), or empty to turn it off. The
// point of having it here rather than only in the environment is that a phone
// has no way to set the environment of the app -- see arm64_faultdump_set.
int proc_ish_show_arm64_faultdump(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    char spec[64];
    arm64_faultdump_show(spec, sizeof(spec));
    if (spec[0] == '\0')
        proc_printf(buf, "off\n");
    else
        proc_printf(buf, "%s\n", spec);
    return 0;
}

int proc_ish_update_arm64_faultdump(struct proc_entry *UNUSED(entry), struct proc_data *data) {
    size_t start = 0;
    size_t end = data->size;

    while (start < end && (data->data[start] == ' ' || data->data[start] == '\t' ||
            data->data[start] == '\r' || data->data[start] == '\n'))
        start++;
    while (end > start && (data->data[end - 1] == ' ' || data->data[end - 1] == '\t' ||
            data->data[end - 1] == '\r' || data->data[end - 1] == '\n'))
        end--;

    size_t len = end - start;
    if (len >= 64)
        return _EINVAL;

    char spec[64];
    memcpy(spec, &data->data[start], len);
    spec[len] = '\0';
    // "off" and "none" spell the empty setting, because echoing an empty string
    // through a shell is awkward enough to be worth not requiring.
    if (strcmp(spec, "off") == 0 || strcmp(spec, "none") == 0)
        spec[0] = '\0';
    // Reject anything that is not digits and commas rather than silently
    // ignoring the unparseable parts: a typo here costs a whole crash
    // reproduction to notice.
    for (size_t i = 0; spec[i] != '\0'; i++)
        if ((spec[i] < '0' || spec[i] > '9') && spec[i] != ',')
            return _EINVAL;
    arm64_faultdump_set(spec);
    return 0;
}

// /proc/ish/arm64_watch -- the store trace. See arm64_watch_configure.
int proc_ish_show_arm64_watch(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    char spec[64];
    arm64_watch_show(spec, sizeof(spec));
    proc_printf(buf, "%s\n", spec);
    return 0;
}

int proc_ish_update_arm64_watch(struct proc_entry *UNUSED(entry), struct proc_data *data) {
    size_t start = 0, end = data->size;
    while (start < end && (data->data[start] == ' ' || data->data[start] == '\t' ||
            data->data[start] == '\r' || data->data[start] == '\n'))
        start++;
    while (end > start && (data->data[end - 1] == ' ' || data->data[end - 1] == '\t' ||
            data->data[end - 1] == '\r' || data->data[end - 1] == '\n'))
        end--;
    size_t len = end - start;
    if (len >= 64)
        return _EINVAL;
    char spec[64];
    memcpy(spec, &data->data[start], len);
    spec[len] = '\0';
    if (strcmp(spec, "off") == 0 || spec[0] == '\0') {
        arm64_watch_configure(NULL);
        return 0;
    }
    // Reject anything unrecognised rather than silently configuring nothing:
    // this gets set once before a reproduction that may be expensive to stage.
    if (strncmp(spec, "all", 3) != 0 && strncmp(spec, "lo16=", 5) != 0 &&
            strncmp(spec, "val=", 4) != 0)
        return _EINVAL;
    arm64_watch_configure(spec);
    return 0;
}

// What the host CPU can actually do, as opposed to what the build assumed.
// One binary ships to every device from an ARMv8.0 iPad up, so "which
// extensions exist here" is a per-device question and has to be answered on
// the device -- and the dispatch A/B in jit/gadgets-aarch64/gadgets.h is the
// standing proof that the right answer differs by 2x across that range.
int proc_ish_show_cpu_features(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct host_cpu_features f;
    hostCpuFeatures(&f);
    if (!f.valid) {
        proc_printf(buf, "host CPU features unavailable (not an ARM host, or the OS would not say)\n");
        return 0;
    }
    proc_printf(buf, "isa_floor %s\n", f.isa[0] != '\0' ? f.isa : "unknown");
    proc_printf(buf, "flagm %s\n",  f.flagm  ? "yes" : "no");
    proc_printf(buf, "flagm2 %s\n", f.flagm2 ? "yes" : "no");
    proc_printf(buf, "lrcpc %s\n",  f.lrcpc  ? "yes" : "no");
    proc_printf(buf, "lrcpc2 %s\n", f.lrcpc2 ? "yes" : "no");
    proc_printf(buf, "lse %s\n",    f.lse    ? "yes" : "no");
    proc_printf(buf, "lse2 %s\n",   f.lse2   ? "yes" : "no");
    proc_printf(buf, "sve %s\n",    f.sve    ? "yes" : "no");
    proc_printf(buf, "sme %s\n",    f.sme    ? "yes" : "no");

    // The build's dispatch choice next to what the CPU supports, because the
    // pair is the actionable part: a build using the cheaper spelling on a
    // core that implements it conservatively is the 2x regression this
    // project already measured once, and it is invisible otherwise.
#if defined(ISH_ARM64_GRET_LDAPR)
    proc_printf(buf, "gret ldapr\n");
    if (!f.lrcpc)
        proc_printf(buf, "gret_MISMATCH this build needs FEAT_LRCPC and this CPU lacks it\n");
#elif defined(ISH_ARM64_GRET_LDAR)
    proc_printf(buf, "gret ldar\n");
#else
    proc_printf(buf, "gret dmb\n");
#endif
    proc_printf(buf, "gret_ldapr_possible %s\n", f.lrcpc ? "yes" : "no");
    return 0;
}
