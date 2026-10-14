#ifndef UTIL_WAIT_SPIN_H
#define UTIL_WAIT_SPIN_H

// Look again for a little while before going to sleep.
//
// A task that has just sent something and waits for the answer -- a client
// and its server, two ends of a pipeline, a thread handing a mutex or a
// condition back and forth -- usually has it within microseconds, and going
// to sleep and being woken costs more than that on its own. Two processes
// passing a byte each way over a pipe, natively on an M5: 4.1us a round trip
// blocked in read(), 7.0us the way AOK waited (read, mask, unmask, poll,
// read), 2.1us when each side retried its read before sleeping.
//
// It only works if the looking lasts about as long as a wake does: with less,
// the peer -- itself asleep -- has not run yet, both sides sleep every time,
// and nothing is gained (a fixed 16 looks at a socket made no difference; 32
// reads of a pipe happened to be just enough on one machine). So it is
// bounded by time, not by count.
//
// And it must not be paid for nothing. A wait whose answer is not coming --
// an idle connection, a worker with no work, more runnable threads than
// cores -- finds nothing, and after finding nothing it skips the looking for
// the next 1, 3, 7 ... 63 waits, until one succeeds again. The state is the
// host thread's, and separate for each kind of wait that includes this file.
//
//     struct wait_spin spin;
//     if (wait_spin_begin(&spin)) {
//         bool got = false;
//         do { got = look(); } while (!got && wait_spin_more(&spin));
//         wait_spin_end(got);
//         if (got) return;
//     }
//     sleep as before
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define WAIT_SPIN_NS 25000

struct wait_spin {
    uint64_t until;
};

static __thread unsigned wait_spin_misses, wait_spin_skip;

static inline uint64_t wait_spin_now(void) {
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
#endif
}

static inline bool wait_spin_begin(struct wait_spin *spin) {
    if (wait_spin_skip > 0) {
        wait_spin_skip--;
        return false;
    }
    spin->until = wait_spin_now() + WAIT_SPIN_NS;
    return true;
}

static inline bool wait_spin_more(struct wait_spin *spin) {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
    return wait_spin_now() < spin->until;
}

static inline void wait_spin_end(bool got) {
    if (got) {
        wait_spin_misses = 0;
        return;
    }
    if (wait_spin_misses < 6)
        wait_spin_misses++;
    wait_spin_skip = (1u << wait_spin_misses) - 1;
}

#endif
