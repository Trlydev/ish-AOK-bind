/*
 * native_applet_race.c -- native applets started at the same moment each run
 * as the applet they were asked for.
 *
 * Native programs are threads of one host process, so what the host's C
 * library keeps in one static place, they share. SmallCLUE picks its applet
 * with basename(argv[0]), and Darwin's basename returns a pointer into one
 * buffer for the whole process: two applets starting together could each
 * read the other's name out of it. That is how `... | sha256sum | cut -c1-32`
 * came to print "sha256sum: unsupported option '-c1-32'" and end a suite leg
 * (twice, on a host starting five suites at once) -- cut's arguments, run by
 * sha256sum. Any pair would do; with rm in it, it is not a wrong message.
 *
 * This starts `true` and `false` together, many times over from several
 * threads, and checks every exit status: a `true` that ran as `false` exits 1,
 * and the other way round 0. Before the fix, on an M-series Mac: one wrong in
 * the 4000 pairs, in each of three runs. basename, dirname and strtok now keep
 * their state per thread for native programs (kernel/native_libc.c).
 *
 * Exits 0 and prints "native_applet_race: PASS" on success.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define SMALLCLUE "/AOK/native/smallclue"
#define TDIR "/tmp/native_applet_race"
#define THREADS 8
#define ROUNDS 500

static atomic_int wrong_true, wrong_false, spawn_failed;

static pid_t start(const char *path, const char *name) {
    pid_t pid = fork();
    if (pid == 0) {
        execl(path, name, (char *) NULL);
        _exit(127);
    }
    return pid;
}

static int status_of(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void *worker(void *arg) {
    (void) arg;
    for (int i = 0; i < ROUNDS; i++) {
        pid_t t = start(TDIR "/true", "true");
        pid_t f = start(TDIR "/false", "false");
        if (t < 0 || f < 0) {
            atomic_fetch_add(&spawn_failed, 1);
            if (t > 0) status_of(t);
            if (f > 0) status_of(f);
            continue;
        }
        if (status_of(t) != 0)
            atomic_fetch_add(&wrong_true, 1);
        if (status_of(f) != 1)
            atomic_fetch_add(&wrong_false, 1);
    }
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (access(SMALLCLUE, X_OK) != 0) {
        printf("native_applet_race: SKIP (no %s)\n", SMALLCLUE);
        return 0;
    }
    alarm(test_watchdog_secs(240));
    mkdir(TDIR, 0755);
    unlink(TDIR "/true");
    unlink(TDIR "/false");
    if (symlink(SMALLCLUE, TDIR "/true") != 0 || symlink(SMALLCLUE, TDIR "/false") != 0) {
        printf("FAIL symlink: %s\n", strerror(errno));
        return 1;
    }
    /* The pair alone first: the test means nothing if they are not what
     * they say. */
    if (status_of(start(TDIR "/true", "true")) != 0 || status_of(start(TDIR "/false", "false")) != 1) {
        printf("FAIL true and false do not exit 0 and 1 even alone\n");
        return 1;
    }
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);
    unlink(TDIR "/true");
    unlink(TDIR "/false");
    rmdir(TDIR);
    int wt = atomic_load(&wrong_true), wf = atomic_load(&wrong_false);
    test_logf("%d pairs: %d true exited nonzero, %d false exited 0, %d not started\n",
              THREADS * ROUNDS, wt, wf, atomic_load(&spawn_failed));
    if (wt != 0 || wf != 0) {
        printf("FAIL of %d pairs started together, %d `true` did not exit 0 and %d `false` did not exit 1\n",
               THREADS * ROUNDS, wt, wf);
        failures_total++;
    }
    if (atomic_load(&spawn_failed) > THREADS * ROUNDS / 10) {
        printf("FAIL %d of %d pairs could not be started\n", atomic_load(&spawn_failed), THREADS * ROUNDS);
        failures_total++;
    }
    return finish_suite("native_applet_race");
}
