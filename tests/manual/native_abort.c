// abort() in a native program ends the program, not the emulator.
//
// A native program (/AOK/native) is code inside the emulator's own process.
// Its abort() used to be the host's: the whole app died, every session with
// it. Build 558 had that from the field -- a Rust panic in native helix
// (tokio met a poll error it does not expect, and the Rust crate is built
// panic=abort). abort() is routed now (kernel/native_libc.c nlibc_abort): the
// program dies of SIGABRT, as a Linux process does, and its parent reads that
// in the wait status.
//
// rust-probe is the native program that can be asked to do it, three ways:
//
//   --panic          a panic on the program's first thread
//   --panic-thread   a panic on a thread it started (where tokio's workers
//                    run, and where the reported panic was)
//   --abort          std::process::abort()
//
// Each must be reported to this test as killed by SIGABRT, with the panic's
// message on the program's stderr for the first two, and this test must
// still be running to say so: before the routing, the emulator died with the
// first of them, and the suite with it. A plain run of the same program
// exits 0, so it is the request that kills it.
//
// SKIPs where there is no rust-probe (a build without the Rust native crate).
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROBE "/AOK/native/rust-probe"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("native_abort: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

// Runs the probe with one argument (or none); its stderr into `err`.
static int run(const char *arg, char *err, size_t err_size) {
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(null, 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execl(PROBE, "rust-probe", arg, (char *) NULL);
        _exit(127);
    }
    close(fds[1]);
    size_t used = 0;
    ssize_t n;
    while (used < err_size - 1 && (n = read(fds[0], err + used, err_size - 1 - used)) > 0)
        used += (size_t) n;
    err[used] = '\0';
    close(fds[0]);
    int status = -1;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    return status;
}

int main(void) {
    if (access(PROBE, X_OK) != 0) {
        printf("native_abort: SKIP (no " PROBE ")\n");
        return 0;
    }
    char err[4096];

    // The control: asked for nothing, it runs and exits 0.
    int status = run(NULL, err, sizeof(err));
    CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "a plain run: status %#x, wanted exit 0", status);

    static const struct { const char *arg; const char *says; } cases[] = {
        {"--panic", "panicking on request"},
        {"--panic-thread", "panicking on a second thread"},
        {"--abort", NULL},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        status = run(cases[i].arg, err, sizeof(err));
        CHECK(status != -1 && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
              "%s: status %#x, wanted killed by SIGABRT", cases[i].arg, status);
        if (cases[i].says != NULL)
            CHECK(strstr(err, cases[i].says) != NULL,
                  "%s: the panic message did not reach the program's stderr (got \"%.200s\")", cases[i].arg, err);
    }

    if (failures != 0) {
        printf("native_abort: FAIL failures=%d\n", failures);
        return 1;
    }
    printf("native_abort: PASS\n");
    return 0;
}
