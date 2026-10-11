#!/bin/bash
# abn.sh <name> <udid> <rounds> <ssh host> "<arch:root> ..." <build>...
#
# ab.sh for any number of builds: each round installs and runs every build in
# turn ($SP/app-<build>/iSH-AOK.app), so one device run can tell several
# changes apart. That is what it is for: on 2026-10-11 three small gadget
# changes measured together came out 2.5% slower on the M4, and it took one
# build each to find that one of them cost 4% and the others were worth having.
# Results: $SP/abn-<name>.log; `python3 tab.py <log>...` prints medians against
# the first build named. Same device setup as ab.sh.
SP=$(cd "$(dirname "$0")" && pwd)
name=$1 udid=$2 rounds=$3 host=$4 spec=$5; shift 5
log=$SP/abn-$name.log
s() { ssh -o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=30 -o ServerAliveCountMax=4 $host "$@"; }
for r in $(seq 1 $rounds); do
  for b in "$@"; do
    until mkdir $SP/install.lock 2>/dev/null; do sleep 5; done
    xcrun devicectl device install app --device $udid $SP/app-$b/iSH-AOK.app >> $log 2>&1
    rmdir $SP/install.lock
    xcrun devicectl device process launch --device $udid --terminate-existing --environment-variables '{"ISH_SESSION_RESUME":"fresh"}' app.ish.iSH-AOK >> $log 2>&1
    n=0; until s true 2>/dev/null || [ $n -ge 60 ]; do sleep 10; n=$((n+1)); done
    sleep 20
    nspec=$(echo $spec | wc -w)
    s "sh ~/bench558/bench.sh r$r-b$b $spec > /dev/null 2>&1; tail -$((nspec + 1)) ~/bench558/results.txt | head -$nspec" >> $log 2>&1
  done
done
echo "ALLDONE $(date +%T)" >> $log
