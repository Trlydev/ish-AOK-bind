#!/bin/sh
# tcp_suspend_reset.sh -- a TCP connection an iOS suspension took away tells
# the guest "connection reset", never "bad file descriptor" (GH #631).
#
# The guest half is tcp_suspend_reset.c, which has the story. This builds it
# in the root and runs it on a CLI build with the app's suspend/resume socket
# cycle driven from a host thread (ISH_SOCKRESTART_AFTER=1) and this process's
# sockets defuncted in between, as iOS does it (ISH_SOCKRESTART_TEST_DESTROY=
# defunct, fs/sockrestart.c). Darwin only: nothing else can defunct a socket.
#
# Then once more with no suspension, as the control: every connection is
# alive, the probe must notice and FAIL. A probe that passed there would be
# passing because nothing happened.
#
#     tests/manual/tcp_suspend_reset.sh [root]
#
# `root` defaults to build/devuan-arm64-test and needs a C compiler. $ISH
# overrides the binary (default build/ish).
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/devuan-arm64-test}
[ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
[ -x "$ISH" ] || { echo "no ish at $ISH" >&2; exit 2; }
if [ "$(uname -s)" != Darwin ]; then
    echo "SKIP: defuncting a socket needs Darwin"
    exit 0
fi
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-tcpreset.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

cp "$REPO/tests/manual/tcp_suspend_reset.c" "$WORK/"
cat > "$WORK/build.sh" <<'EOF'
cc -O1 -pthread -o /realmnt/probe /realmnt/tcp_suspend_reset.c
EOF
ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh /realmnt/build.sh < /dev/null \
    || { echo "FAIL: could not build the probe"; exit 1; }

# run NAME [VAR=value ...]: the probe under that environment, 40 s at most.
run() {
    name=$1; shift
    env "$@" ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /realmnt/probe < /dev/null > "$WORK/$name.out" 2>&1 &
    pid=$!
    n=0
    while kill -0 $pid 2>/dev/null && [ $n -lt 400 ]; do sleep 0.1; n=$((n + 1)); done
    if kill -0 $pid 2>/dev/null; then
        kill -9 $pid 2>/dev/null
        wait $pid 2>/dev/null
        echo "FAIL: $name hung for 40s"
        sed 's/^/    | /' "$WORK/$name.out"
        return 2
    fi
    wait $pid
}

failures=0
run suspended ISH_SOCKRESTART_AFTER=1 ISH_SOCKRESTART_TEST_DESTROY=defunct
if grep -qx PASS "$WORK/suspended.out" && grep -q "defuncted this process's sockets\|sockrestart: saved" "$WORK/suspended.out"; then
    echo "ok: after the suspension every call reports the connection reset"
else
    echo "FAIL: after the suspension"
    sed 's/^/    | /' "$WORK/suspended.out"
    failures=$((failures + 1))
fi

run control
if grep -q "^FAIL .*still alive" "$WORK/control.out" && ! grep -qx PASS "$WORK/control.out"; then
    echo "ok: with no suspension the probe sees live connections and fails (the control)"
else
    echo "FAIL: the control -- with no suspension the probe should have failed on live connections"
    sed 's/^/    | /' "$WORK/control.out"
    failures=$((failures + 1))
fi

if [ $failures -ne 0 ]; then
    echo "FAIL: $failures case(s)"
    exit 1
fi
echo "PASS: a connection taken by a suspension reads as reset"
