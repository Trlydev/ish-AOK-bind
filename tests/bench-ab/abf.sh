#!/bin/bash
# abf.sh <name> <udid> <rounds> "<ssh host args>" <arch:root> <app> <setting>... : install one app, A/B its fuse settings on the device
SP=$(cd "$(dirname "$0")" && pwd)
name=$1 udid=$2 rounds=$3 host=$4 spec=$5 app=$6; shift 6
log=$SP/abf-$name.log
s() { ssh -o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=30 -o ServerAliveCountMax=4 $host "$@"; }
until mkdir $SP/install.lock 2>/dev/null; do sleep 5; done
xcrun devicectl device install app --device $udid $SP/app-$app/iSH-AOK.app >> $log 2>&1
rmdir $SP/install.lock
xcrun devicectl device process launch --device $udid --terminate-existing --environment-variables '{"ISH_SESSION_RESUME":"fresh"}' app.ish.iSH-AOK >> $log 2>&1
n=0; until s true 2>/dev/null || [ $n -ge 60 ]; do sleep 10; n=$((n+1)); done
sleep 20
s 'cat > ~/bench558/benchf.sh' < $SP/benchf.sh
n=$(( rounds * $# ))
s "sh ~/bench558/benchf.sh $name $rounds $spec $* > /dev/null 2>&1; tail -$((n + 1)) ~/bench558/results.txt | head -$n" >> $log 2>&1
echo "ALLDONE $(date +%T)" >> $log
