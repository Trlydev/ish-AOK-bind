#!/bin/bash
# abn.sh <name> <udid> <rounds> <ssh host> "<arch:root> ..." <build>...
#
# ab.sh for any number of builds: each round installs and runs every build in
# turn ($SP/app-<build>/iSH-AOK.app), so one device run can tell several
# changes apart. That is what it is for: on 2026-10-11 three small gadget
# changes measured together came out 2.5% slower on the M4, and it took one
# build each to find that one of them cost 4% and the others were worth having.
# Results: $SP/abn-<name>.log; `python3 tab.py <log>...` prints medians against
# the first build named. Same device setup as ab.sh. It stops, with a REFUSED
# line, on a device below 40% battery or in Low Power Mode, or one whose guest
# does not come back after a launch (lib.sh).
SP=$(cd "$(dirname "$0")" && pwd)
name=$1 udid=$2 rounds=$3 host=$4 spec=$5; shift 5
log=$SP/abn-$name.log
. "$SP/lib.sh"
battery_ok || exit 3
nspec=$(echo $spec | wc -w)
for r in $(seq 1 $rounds); do
  for b in "$@"; do
    install_and_launch $b || exit 3
    s "sh ~/bench558/bench.sh r$r-b$b $spec > /dev/null 2>&1; tail -$((nspec + 1)) ~/bench558/results.txt | head -$nspec" >> $log 2>&1
    refused && exit 3
  done
done
echo "ALLDONE $(date +%T)" >> $log
