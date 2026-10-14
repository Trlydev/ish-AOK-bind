#!/bin/bash
# cryptoab.sh <name> <udid> <rounds> "<ssh host args>" <build>...
#
# abn.sh for public-key work: each round installs every build in turn and runs
# `openssl speed` in the BOOTED root (it needs openssl there) -- RSA-2048,
# ECDSA P-256, SHA-256 -- writing openssl's own last lines to
# $SP/crypto-<name>.log, one line per build and round. RSA's columns are sign
# and verify times, then keygens, signs and verifies a second. It is how the
# arm64 guest's add-with-carry and multiply gadgets were found wanting and
# then timed (2026-10-12: RSA signing 101 -> 192 a second on the M4). The usual
# benchmarks have no bignum arithmetic in them at all.
SP=$(cd "$(dirname "$0")" && pwd)
name=$1 udid=$2 rounds=$3 host=$4; shift 4
log=$SP/crypto-$name.log
. "$SP/lib.sh"
battery_ok || exit 3
for r in $(seq 1 $rounds); do
  for b in "$@"; do
    install_and_launch $b || exit 3
    out=$(s 'grep -q "^low_power_mode: Enabled" /proc/ish/BAT0 2>/dev/null && echo "REFUSED: Low Power Mode is on"; openssl speed -seconds 2 rsa2048 2>/dev/null | tail -1; openssl speed -seconds 2 ecdsap256 2>/dev/null | tail -1; openssl speed -seconds 2 -evp sha256 2>/dev/null | tail -1' 2>&1 | tr '\n' '|')
    case "$out" in REFUSED*) echo "$out" | tr '|' '\n' | head -1 | tee -a "$log" >&2; exit 3 ;; esac
    echo "r$r-b$b $out" >> "$log"
  done
done
echo "ALLDONE $(date +%T)" >> "$log"
