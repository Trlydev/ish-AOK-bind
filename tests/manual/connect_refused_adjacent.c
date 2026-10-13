// A TCP connect to a loopback port nobody listens on is ECONNREFUSED, the
// first time as every time.
//
// It was EINVAL the first time when the port was the one next to a port just
// bound: macOS's own connect() answers EINVAL there (the reset comes back
// before the call has got as far as waiting, and it finds the socket already
// disconnected), and AOK passed that on. A client that tries the next port
// up, or probes for a free one, was told its arguments were wrong. Found by
// tests/manual/netns_basic.c, which asks whether a sandbox's listener can be
// reached from outside and was told "Invalid argument".
//
// Each round binds a fresh listener (so the kernel hands out a fresh port),
// then connects to the two ports above it. A port there that happens to be in
// use is skipped; the round still has the other.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    int failures = 0, asked = 0;
    for (int round = 0; round < 12; round++) {
        int l = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        socklen_t len = sizeof(addr);
        if (l < 0 || bind(l, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(l, 4) != 0 ||
                getsockname(l, (struct sockaddr *) &addr, &len) != 0) {
            printf("connect_refused_adjacent: FAIL setup: %s\n", strerror(errno));
            return 1;
        }
        int port = ntohs(addr.sin_port);
        for (int up = 1; up <= 2; up++) {
            struct sockaddr_in to = addr;
            to.sin_port = htons((unsigned short) (port + up));
            int c = socket(AF_INET, SOCK_STREAM, 0);
            errno = 0;
            int r = connect(c, (struct sockaddr *) &to, sizeof(to));
            int e = errno;
            close(c);
            if (r == 0)
                continue;   // somebody has that port
            asked++;
            if (e != ECONNREFUSED) {
                printf("connect_refused_adjacent: round %d, port +%d: errno %d (%s), wanted ECONNREFUSED\n",
                       round, up, e, strerror(e));
                failures++;
            }
        }
        close(l);
    }
    if (failures != 0 || asked == 0) {
        printf("connect_refused_adjacent: FAIL failures=%d of %d connects\n", failures, asked);
        return 1;
    }
    printf("connect_refused_adjacent: PASS (%d connects)\n", asked);
    return 0;
}
