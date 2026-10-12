#!/bin/sh
# bench.sh <label> <arch:root> ... -- run in the booted root as mke; appends to ~/bench558/results.txt
# Per root: 7-Zip's single-thread benchmark (`7z b 1 -mmt1 -md22`, its Tot MIPS)
# and pybench.py. A root of `booted` runs in the booted root; any other is entered
# with mount-root.sh. Each run waits for /proc/ish/thermal_state to read nominal
# and records it either side: a phone under sustained load throttles 2-3x. It
# refuses a device in Low Power Mode or below 40% battery (power_ok), and
# records the battery with each result.
label=$1; shift
mkdir -p ~/bench558
# Not on a throttled device, or one about to be (tests/bench-ab/lib.sh has the
# day this is from): Low Power Mode caps the CPU, and below 40% a device that is
# being timed is on its way there even on a charger. /proc/ish/BAT0 is the
# host's own reading; a host without one (the Mac) is not refused.
power_ok() {
    power=""
    [ -r /proc/ish/BAT0 ] || return 0
    lpm=$(awk '/^low_power_mode:/{print $2}' /proc/ish/BAT0)
    lvl=$(awk '/^battery_level:/{print int($2)}' /proc/ish/BAT0)
    st=$(awk '/^battery_state:/{print $2}' /proc/ish/BAT0)
    power="bat=${lvl}%/${st} lpm=${lpm}"
    why=""
    [ "$lpm" = Enabled ] && why="Low Power Mode is on"
    [ -z "$why" ] && [ -n "$lvl" ] && [ "$lvl" -ge 0 ] && [ "$lvl" -lt 40 ] && why="battery below 40%"
    [ -z "$why" ] && return 0
    echo "REFUSED $label: $why ($power)" | tee -a ~/bench558/results.txt
    return 1
}
build=$(dmesg | grep -m1 -o 'built [A-Z][a-z]* [0-9]* [0-9]* [0-9:]*')
for spec in "$@"; do
    arch=${spec%%:*} root=${spec#*:}
    power_ok || exit 3
    # wait (up to 15 min) for the device to cool to nominal; record the state either side
    n=0; while [ "$(cat /proc/ish/thermal_state 2>/dev/null || echo nominal)" != nominal ] && [ $n -lt 30 ]; do sleep 30; n=$((n+1)); done
    th0=$(cat /proc/ish/thermal_state 2>/dev/null)
    if [ "$root" = booted ]; then run() { sh -c "$1"; }; else run() { sudo -n /AOK/tools/mount-root.sh "$root" -- sh -c "$1"; }; fi
    sz=$(run '7z b 1 -mmt1 -md22 2>&1' < /dev/null | awk '/^Tot:/{print $NF}' | tail -1)
    py=$(run 'python3 /AOK/fakefs/bench/pybench.py 2>&1' < /dev/null | tail -1)
    th1=$(cat /proc/ish/thermal_state 2>/dev/null)
    echo "$(date -u +%H:%M:%S) $label [$build] $arch $root 7z=$sz $py thermal=$th0/$th1 $power" >> ~/bench558/results.txt
done
echo DONE $label >> ~/bench558/results.txt
