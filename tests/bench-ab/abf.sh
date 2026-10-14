#!/bin/bash
# abf.sh <name> <udid> <rounds> "<ssh host args>" <arch:root> <app> <setting>...
#
# A/B a JIT pass inside ONE app build: install $SP/app-<app> once, then have the
# device alternate settings of /proc/ish/<arch>_jit_fuse between runs
# (benchf.sh), e.g. `pair=0 pair=1`. No reinstall between arms and the same
# gadget layout in both. Results: $SP/abf-<name>.log, in bench.sh's lines with
# the setting as the build tag, so tab.py reads them. A switch that costs
# something when it is OFF flatters what it switches: check a keeper against the
# build before it with abn.sh too. Stops with a REFUSED line as abn.sh does.
SP=$(cd "$(dirname "$0")" && pwd)
name=$1 udid=$2 rounds=$3 host=$4 spec=$5 app=$6; shift 6
log=$SP/abf-$name.log
. "$SP/lib.sh"
battery_ok || exit 3
install_and_launch $app || exit 3
n=$(( rounds * $# ))
s "sh ~/bench558/benchf.sh $name $rounds $spec $* > /dev/null 2>&1; tail -$((n + 1)) ~/bench558/results.txt | head -$n" >> $log 2>&1
refused && exit 3
echo "ALLDONE $(date +%T)" >> $log
