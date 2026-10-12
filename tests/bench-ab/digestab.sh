#!/bin/bash
# digestab.sh <name> <udid> <rounds> "<ssh host args>" <root> <build>...
#
# abn.sh for hashing: each round installs every build in turn and times the
# digest tools (md5, sha1, sha256, sha512, blake2b, cksum) with digest.sh, in
# <root> (`booted`, or a root's name as in abn.sh's arch:root), one line per
# build and round in $SP/digest-<name>.log, milliseconds, lower is better. The usual benchmarks rotate nothing; a
# digest is little else, and that is how the arm64 guest's generic rotate
# and shifted-operand gadgets were found (2026-10-12).
SP=$(cd "$(dirname "$0")" && pwd)
name=$1 udid=$2 rounds=$3 host=$4 root=$5; shift 5
log=$SP/digest-$name.log
. "$SP/lib.sh"
battery_ok || exit 3
for r in $(seq 1 $rounds); do
  for b in "$@"; do
    install_and_launch $b || exit 3
    s 'cat > ~/bench558/digest.sh' < "$SP/digest.sh"
    out=$(s "sh ~/bench558/digest.sh r$r-b$b $root" 2>&1 | tail -1)
    case "$out" in REFUSED*) echo "$out" | tee -a "$log" >&2; exit 3 ;; esac
    echo "$out" >> "$log"
  done
done
echo "ALLDONE $(date +%T)" >> "$log"
