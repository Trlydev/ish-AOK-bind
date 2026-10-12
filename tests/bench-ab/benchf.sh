#!/bin/sh
# benchf.sh <label> <rounds> <arch:root> <setting>... -- one build, alternated
# settings of /proc/ish/<arch>_jit_fuse (e.g. pair=0 pair=1 for arm64, fuse2=0
# fuse2=1 for riscv64); appends to ~/bench558/results.txt in bench.sh's
# format, the setting as the build tag.
label=$1 rounds=$2 spec=$3; shift 3
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
arch=${spec%%:*} root=${spec#*:}
case $arch in x86_64) fuse=amd64_jit_fuse ;; *) fuse=${arch}_jit_fuse ;; esac
run() { sudo -n /AOK/tools/mount-root.sh "$root" -- sh -c "$1"; }
r=1
while [ $r -le $rounds ]; do
    for set in "$@"; do
        power_ok || { sudo -n sh -c "echo all=1 > /proc/ish/$fuse"; exit 3; }
        sudo -n sh -c "echo $set > /proc/ish/$fuse" || echo "SETFAIL $set" >> ~/bench558/results.txt
        tag=$(echo "$set" | tr -d '=, ')
        sz=$(run '7z b 1 -mmt1 -md22 2>&1' < /dev/null | awk '/^Tot:/{print $NF}' | tail -1)
        py=$(run 'python3 /AOK/fakefs/bench/pybench.py 2>&1' < /dev/null | tail -1)
        echo "$(date -u +%H:%M:%S) $label-r$r-b$tag [$build] $arch $root 7z=$sz $py $power" >> ~/bench558/results.txt
    done
    r=$((r + 1))
done
sudo -n sh -c "echo all=1 > /proc/ish/$fuse"
echo DONE $label >> ~/bench558/results.txt
