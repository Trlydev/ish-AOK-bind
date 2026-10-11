#!/bin/sh
# benchf.sh <label> <rounds> <arch:root> <setting>... -- one build, alternated
# settings of /proc/ish/<arch>_jit_fuse (e.g. pair=0 pair=1 for arm64, fuse2=0
# fuse2=1 for riscv64); appends to ~/bench558/results.txt in bench.sh's
# format, the setting as the build tag.
label=$1 rounds=$2 spec=$3; shift 3
mkdir -p ~/bench558
build=$(dmesg | grep -m1 -o 'built [A-Z][a-z]* [0-9]* [0-9]* [0-9:]*')
arch=${spec%%:*} root=${spec#*:}
case $arch in x86_64) fuse=amd64_jit_fuse ;; *) fuse=${arch}_jit_fuse ;; esac
run() { sudo -n /AOK/tools/mount-root.sh "$root" -- sh -c "$1"; }
r=1
while [ $r -le $rounds ]; do
    for set in "$@"; do
        sudo -n sh -c "echo $set > /proc/ish/$fuse" || echo "SETFAIL $set" >> ~/bench558/results.txt
        tag=$(echo "$set" | tr -d '=, ')
        sz=$(run '7z b 1 -mmt1 -md22 2>&1' < /dev/null | awk '/^Tot:/{print $NF}' | tail -1)
        py=$(run 'python3 /AOK/fakefs/bench/pybench.py 2>&1' < /dev/null | tail -1)
        echo "$(date -u +%H:%M:%S) $label-r$r-b$tag [$build] $arch $root 7z=$sz $py" >> ~/bench558/results.txt
    done
    r=$((r + 1))
done
sudo -n sh -c "echo all=1 > /proc/ish/$fuse"
echo DONE $label >> ~/bench558/results.txt
