#!/bin/sh
# native_tokio_poll_error.sh -- tokio's reactor in a native Rust program: two
# programs at once keep to their own queues, a failed poll is retried, and one
# that keeps failing ends the program, never the emulator.
#
# tokio's reactor in a native program is not the kernel's kqueue but AOK's
# shim over the guest's ppoll (kernel/native_kqueue.c), and tokio panics on
# any poll error it does not expect. Build 558 had that from the field: native
# helix on an iPad, a panic in runtime::io::Driver::turn, and -- the Rust
# crate being panic=abort and abort() then the host's -- the app gone.
#
# Two things changed, and this checks both:
#   - the tokio fork (deps/tokio, aok-embedded) takes a failed poll for a
#     spurious wake-up and looks again after a pause, giving up after about
#     two seconds of nothing but failures;
#   - giving up is a panic, a panic is an abort, and abort() in a native
#     program is the program's (kernel/native_libc.c nlibc_abort): it dies of
#     SIGABRT and the shell that ran it carries on.
#
# What most likely made that poll fail is here too. The shim kept its queues
# in one table for the whole emulator, found by descriptor NUMBER -- and every
# tokio program's queue is the same small number. Two at once (two hx
# sessions) registered in and woke each other's queues, and when one of them
# closed its descriptors the other was left asking for a queue that was no
# longer listed: EBADF from kevent(), tokio's panic. A queue now belongs to
# the program run that made it.
#
# And one more that the same probe showed, with no second program and no
# failure injected: a descriptor the shim had reported ready was left out of
# the wait until a LATER kevent() saw it drained, so a waiter that went back
# to sleep before the consumer read it never saw the next data or the peer's
# close. The probe's TCP round trip hung on that every time. The shim's read
# and write paths now say when a descriptor has been drained.
#
# Two more came out of running pairs of programs in a loop, each about one
# time in fifty: a descriptor registered while the reactor thread was already
# asleep was not in that sleeper's set and nothing woke it (a hang); and two
# threads of one program collecting the same pending signal at once were two
# sigtimedwaits on one task, which is a kernel assertion (the emulator's
# abort). Registering wakes the sleeper now, and one thread of a program at a
# time collects its signals. Loop the pair case to look for either:
# 360 pairs ran clean on two roots after the fixes.
#
# ISH_KQ_TEST_FAIL=<n> makes the shim fail its next n waits (ENOMEM), which
# nothing else can do on demand. Runs of rust-probe's tokio section (a
# timer, a TCP round trip, a child process):
#
#   no failures       the section runs to the end (it hung in the round trip)
#   two at once       eight pairs of programs started together: every one
#                     runs to the end
#   5 failures        it still runs, every line of it
#   failures for ever the program is killed by SIGABRT with tokio's message
#                     on its stderr, and the next command in the same guest
#                     shell still runs
#
# Needs a CLI built with the probe's tokio section, which a default build
# does not have:
#     meson setup build-tokio -Dnative_rust_features=tokio-probe
#     ninja -C build-tokio ish
#     ISH=build-tokio/ish tests/manual/native_tokio_poll_error.sh [root]
# With any other binary this SKIPs. `root` defaults to build/alpine-arm64-324.
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ISH=${ISH:-$REPO/build/ish}
ROOT=${1:-$REPO/build/alpine-arm64-324}
[ -d "$ROOT" ] || { echo "no root at $ROOT" >&2; exit 2; }
[ -x "$ISH" ] || { echo "no ish at $ISH" >&2; exit 2; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/aok-tokiopoll.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/run.sh" <<'EOF'
/AOK/native/rust-probe > /realmnt/out.txt 2> /realmnt/err.txt
echo "status $?" > /realmnt/status.txt
echo alive >> /realmnt/status.txt
EOF
# Two programs started together, eight times over; one line per program.
cat > "$WORK/pair.sh" <<'EOF'
: > /realmnt/out.txt
: > /realmnt/err.txt
i=0
while [ $i -lt 8 ]; do
    /AOK/native/rust-probe > /tmp/pair-a.out 2>> /realmnt/err.txt &
    a=$!
    /AOK/native/rust-probe > /tmp/pair-b.out 2>> /realmnt/err.txt &
    b=$!
    wait $a; sa=$?
    wait $b; sb=$?
    for p in a b; do
        if [ $p = a ]; then st=$sa; else st=$sb; fi
        if grep -q 'io driver: port=.*echoed "ASYNC ROUND TRIP"' /tmp/pair-$p.out && [ $st = 0 ]; then
            echo "pair $i $p ok" >> /realmnt/out.txt
        else
            echo "pair $i $p BAD status=$st: $(tail -1 /tmp/pair-$p.out)" >> /realmnt/out.txt
        fi
    done
    i=$((i + 1))
done
echo "status 0" > /realmnt/status.txt
echo alive >> /realmnt/status.txt
EOF

# run NAME [VAR=value ...]: one guest shell running the probe, two minutes at
# most; leaves NAME.out, NAME.err and NAME.status.
run() {
    name=$1; shift
    rm -f "$WORK/out.txt" "$WORK/err.txt" "$WORK/status.txt"
    env "$@" ISH_REAL_MNT=$WORK "$ISH" -f "$ROOT" /bin/sh /realmnt/${SCRIPT:-run.sh} < /dev/null > "$WORK/$name.emu" 2>&1 &
    pid=$!
    n=0
    while kill -0 $pid 2>/dev/null && [ $n -lt 1200 ]; do sleep 0.1; n=$((n + 1)); done
    if kill -0 $pid 2>/dev/null; then
        kill -9 $pid 2>/dev/null
        wait $pid 2>/dev/null
        echo "hung" > "$WORK/$name.rc"
    else
        wait $pid
        echo $? > "$WORK/$name.rc"
    fi
    for f in out err status; do
        [ -f "$WORK/$f.txt" ] && mv "$WORK/$f.txt" "$WORK/$name.$f" || : > "$WORK/$name.$f"
    done
}
show() { for f in rc status out err emu; do sed "s/^/    | $f: /" "$WORK/$1.$f" 2>/dev/null | tail -12; done; }
ran_whole() { # the tokio section's three results, and a clean exit
    grep -q "time driver: sleep(150ms) returned" "$WORK/$1.out" &&
    grep -q 'io driver: port=.*echoed "ASYNC ROUND TRIP"' "$WORK/$1.out" &&
    grep -qx "status 0" "$WORK/$1.status" && grep -qx alive "$WORK/$1.status" &&
    [ "$(cat "$WORK/$1.rc")" = 0 ]
}

run control
if ! grep -q "^tokio:" "$WORK/control.out"; then
    echo "SKIP: $ISH has no tokio probe (build with -Dnative_rust_features=tokio-probe)"
    exit 0
fi
failures=0
if ran_whole control; then
    echo "ok: no failures -- the tokio section runs to the end"
else
    echo "FAIL: no failures"; show control; failures=$((failures + 1))
fi

SCRIPT=pair.sh
run pair
SCRIPT=run.sh
if [ "$(grep -c ' ok$' "$WORK/pair.out")" = 16 ] && ! grep -q BAD "$WORK/pair.out" &&
        grep -qx alive "$WORK/pair.status" && [ "$(cat "$WORK/pair.rc")" = 0 ]; then
    echo "ok: two at once, eight times -- all sixteen run to the end"
else
    echo "FAIL: two at once"; show pair; failures=$((failures + 1))
fi

run five ISH_KQ_TEST_FAIL=5
if ran_whole five; then
    echo "ok: five failed polls -- retried, and the section runs to the end"
else
    echo "FAIL: five failed polls"; show five; failures=$((failures + 1))
fi

run forever ISH_KQ_TEST_FAIL=1000000
if grep -qx "status 134" "$WORK/forever.status" && grep -qx alive "$WORK/forever.status" &&
        grep -q "unexpected error when polling the I/O driver" "$WORK/forever.err" &&
        [ "$(cat "$WORK/forever.rc")" = 0 ]; then
    echo "ok: polls that never succeed -- the program dies of SIGABRT with tokio's message, the shell and the emulator go on"
else
    echo "FAIL: polls that never succeed"; show forever; failures=$((failures + 1))
fi

if [ $failures -ne 0 ]; then
    echo "FAIL: $failures case(s)"
    exit 1
fi
echo "PASS: programs keep to their own queues, a failed poll is retried, and a dead reactor ends only its program"
