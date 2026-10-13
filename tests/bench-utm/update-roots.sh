#!/bin/bash
# Pushes changed benchmark scripts into both AOK roots and rebuilds the QEMU initramfs images from them.
set -euo pipefail
export PATH=/opt/homebrew/bin:$PATH COPYFILE_DISABLE=1
W=$HOME/aok-vm-bench; ISH=$HOME/git/ish-AOK/build/ish
# fptest is compiled in the AOK checkout's own test roots, which have a compiler: installing one into
# the benchmark root means a 200 MB download per arch through the guest, and apk has hung on it.
DEV=$HOME/git/ish-AOK/build
for a in aarch64 riscv64; do
    R=$W/root-$a
    case $a in aarch64) dev=$DEV/alpine-arm64-324 ;; riscv64) dev=$DEV/alpine-riscv64-test ;; esac
    if [ ! -s "$W/bench/fptest.$a" ] || [ "$W/bench/fptest.c" -nt "$W/bench/fptest.$a" ]; then
        "$ISH" -f "$dev" /bin/sh -e -c 'mkdir -p /tmp/bench-utm && cd /tmp/bench-utm && cat > fptest.c &&
            gcc -O1 -static -ffp-contract=off -fno-math-errno -o fptest fptest.c -lm && cat fptest' \
            < "$W/bench/fptest.c" > "$W/bench/fptest.$a.tmp"
        mv "$W/bench/fptest.$a.tmp" "$W/bench/fptest.$a"
    fi
    "$ISH" -f "$R" /bin/sh -c 'cat > /bench/fptest && chmod +x /bench/fptest' < "$W/bench/fptest.$a"
    # memtest: -O2, and no -mstrict-align games: one load or store instruction per access is the point.
    if [ ! -s "$W/bench/memtest.$a" ] || [ "$W/bench/memtest.c" -nt "$W/bench/memtest.$a" ]; then
        "$ISH" -f "$dev" /bin/sh -e -c 'mkdir -p /tmp/bench-utm && cd /tmp/bench-utm && cat > memtest.c &&
            gcc -O2 -static -o memtest memtest.c && cat memtest' \
            < "$W/bench/memtest.c" > "$W/bench/memtest.$a.tmp"
        mv "$W/bench/memtest.$a.tmp" "$W/bench/memtest.$a"
    fi
    "$ISH" -f "$R" /bin/sh -c 'cat > /bench/memtest && chmod +x /bench/memtest' < "$W/bench/memtest.$a"
    # simdtest (aarch64 only): every FP and SIMD encoding on a few inputs, for an engine that runs them
    # another way than its reference does (the QEMU plug-in's AArch64 front end runs them on the host).
    if [ "$a" = aarch64 ]; then
        if [ ! -s "$W/bench/simdtest.$a" ] || [ "$W/bench/simdtest.c" -nt "$W/bench/simdtest.$a" ]; then
            "$ISH" -f "$dev" /bin/sh -e -c 'mkdir -p /tmp/bench-utm && cd /tmp/bench-utm && cat > simdtest.c &&
                gcc -O2 -static -o simdtest simdtest.c && cat simdtest' \
                < "$W/bench/simdtest.c" > "$W/bench/simdtest.$a.tmp"
            mv "$W/bench/simdtest.$a.tmp" "$W/bench/simdtest.$a"
        fi
        "$ISH" -f "$R" /bin/sh -c 'cat > /bench/simdtest && chmod +x /bench/simdtest' < "$W/bench/simdtest.$a"
    fi
    tar -cf - -C "$W" bench/init bench/run1.sh bench/check.sh bench/fptest.c bench/memtest.c bench/udhcpc.sh bench/gltest.sh | "$ISH" -f "$R" /bin/sh -c 'tar -xf - -C /'
    "$ISH" -f "$R" /bin/sh -e -c '
        cd /bench && rm -f ._*
        [ -f data2.bin ] || head -c 2097152 data.bin > data2.bin
        [ -f data1.bin ] || head -c 1048576 data.bin > data1.bin
        cp init /init && chmod +x /init run.sh run1.sh check.sh udhcpc.sh gltest.sh
        cd / && rm -f /bench-root.tar
        tar -cf /bench-root.tar bin etc home lib media mnt opt root run sbin srv usr var bench init
    '
    bsdtar -c --format=newc -f "$W/initramfs-$a.cpio" "@$R/data/bench-root.tar"
    ls -la "$W/initramfs-$a.cpio"
done
# For a VM front end (tests/bench-utm/utm): the riscv64 image followed by a second archive with the kernel
# package's modules, which the kernel unpacks over the first. /init loads drivers only when they are there,
# so the race's image is the same guest without them.
M=$W/dl-kernel/modules-riscv64
if [ ! -f "$W/modules-riscv64.cpio" ]; then
    rm -rf "$M" && mkdir -p "$M"
    bsdtar -xf "$W/dl-kernel/linux-lts-riscv64.apk" -C "$M" lib/modules
    (cd "$M" && bsdtar -c --format=newc --uid 0 --gid 0 -f "$W/modules-riscv64.cpio" lib)
fi
# And a third, when ~/aok-vm-bench/aok-tools holds them: iSH-AOK's own provisioning and Wayland scripts
# (opt/AOK/tools/provision-ultimate-alpine.sh, setup-wayland.sh, setup-wayland-extras.sh, start-wayland.sh)
# at /AOK/tools, where the same distribution has them under the app. They know a machine of its own when
# they are on one. A separate archive because /AOK inside an AOK root is the app's own file system: the
# root cannot hold files there. This guest lives in memory, so what they install is gone at shutdown.
T=$W/dl-kernel/aoktools
rm -rf "$T" "$W/aoktools-riscv64.cpio"
if ls "$W"/aok-tools/*.sh > /dev/null 2>&1; then   # (and aok-tools/wayland, the release guard's source)
    mkdir -p "$T/AOK/tools"
    for f in "$W"/aok-tools/*; do
        case $f in */provision-ultimate-devuan.sh) ;; *) cp -r "$f" "$T/AOK/tools/" ;; esac
    done
    chmod -R u+rwX,go+rX "$T/AOK/tools"
    (cd "$T" && bsdtar -c --format=newc --uid 0 --gid 0 -f "$W/aoktools-riscv64.cpio" AOK)
fi
cat "$W/initramfs-riscv64.cpio" "$W/modules-riscv64.cpio" $(ls "$W/aoktools-riscv64.cpio" 2>/dev/null) \
    > "$W/initramfs-riscv64-vm.cpio"
ls -la "$W/modules-riscv64.cpio" "$W/initramfs-riscv64-vm.cpio"
echo UPDATE-DONE
