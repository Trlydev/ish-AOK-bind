// Task-switch costs: round trips between two tasks that take turns.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// two processes, a byte each way through fds
static double pingpong_fds(int a[2], int b[2], int n) {
    pid_t pid = fork();
    char c = 'x';
    if (pid == 0) {
        for (int i = 0; i < n; i++) {
            if (read(a[0], &c, 1) != 1) _exit(1);
            if (write(b[1], &c, 1) != 1) _exit(1);
        }
        _exit(0);
    }
    uint64_t t0 = now();
    for (int i = 0; i < n; i++) {
        if (write(a[1], &c, 1) != 1) break;
        if (read(b[0], &c, 1) != 1) break;
    }
    uint64_t t1 = now();
    waitpid(pid, NULL, 0);
    return (double) (t1 - t0) / n;
}

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int turn, rounds;

static void *cond_peer(void *arg) {
    (void) arg;
    pthread_mutex_lock(&m);
    for (int i = 0; i < rounds; i++) {
        while (turn != 1) pthread_cond_wait(&cv, &m);
        turn = 0;
        pthread_cond_signal(&cv);
    }
    pthread_mutex_unlock(&m);
    return NULL;
}

static double pingpong_cond(int n) {
    pthread_t t;
    rounds = n; turn = 0;
    pthread_create(&t, NULL, cond_peer, NULL);
    uint64_t t0 = now();
    pthread_mutex_lock(&m);
    for (int i = 0; i < n; i++) {
        turn = 1;
        pthread_cond_signal(&cv);
        while (turn != 0) pthread_cond_wait(&cv, &m);
    }
    pthread_mutex_unlock(&m);
    uint64_t t1 = now();
    pthread_join(t, NULL);
    return (double) (t1 - t0) / n;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 20000;
    uint64_t t0 = now();
    for (int i = 0; i < 200000; i++) getppid();
    printf("getppid (a syscall that does nothing):  %8.0f ns\n", (double) (now() - t0) / 200000);
    int p[2];
    if (pipe(p)) return 1;
    char c = 'x';
    t0 = now();
    for (int i = 0; i < 100000; i++) { if (write(p[1], &c, 1) != 1 || read(p[0], &c, 1) != 1) return 1; }
    printf("pipe write+read, one task (no switch):  %8.0f ns\n", (double) (now() - t0) / 100000);
    int a[2], b[2];
    if (pipe(a) || pipe(b)) return 1;
    printf("pipe round trip, two processes:         %8.0f ns\n", pingpong_fds(a, b, n));
    int sa[2], sb[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sa) || socketpair(AF_UNIX, SOCK_STREAM, 0, sb)) return 1;
    int xa[2] = {sa[0], sa[1]}, xb[2] = {sb[0], sb[1]};
    printf("unix socket round trip, two processes:  %8.0f ns\n", pingpong_fds(xa, xb, n));
    printf("condvar round trip, two threads:        %8.0f ns\n", pingpong_cond(n));
    t0 = now();
    for (int i = 0; i < 20000; i++) { pid_t pid = fork(); if (pid == 0) _exit(0); waitpid(pid, NULL, 0); if (i == 499) break; }
    printf("fork + exit + wait:                     %8.0f ns\n", (double) (now() - t0) / 500);
    return 0;
}
