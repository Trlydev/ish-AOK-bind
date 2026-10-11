#!/bin/bash
# Pushes changed benchmark scripts into both AOK roots and rebuilds the QEMU initramfs images from them.
set -euo pipefail
export PATH=/opt/homebrew/bin:$PATH COPYFILE_DISABLE=1
W=$HOME/aok-vm-bench; ISH=$HOME/git/ish-AOK/build/ish
for a in aarch64 riscv64; do
    R=$W/root-$a
    tar -cf - -C "$W" bench/init bench/run1.sh | "$ISH" -f "$R" /bin/sh -c 'tar -xf - -C /'
    "$ISH" -f "$R" /bin/sh -e -c '
        cd /bench && rm -f ._*
        [ -f data2.bin ] || head -c 2097152 data.bin > data2.bin
        [ -f data1.bin ] || head -c 1048576 data.bin > data1.bin
        cp init /init && chmod +x /init run.sh run1.sh
        cd / && rm -f /bench-root.tar
        tar -cf /bench-root.tar bin etc home lib media mnt opt root run sbin srv usr var bench init
    '
    bsdtar -c --format=newc -f "$W/initramfs-$a.cpio" "@$R/data/bench-root.tar"
    ls -la "$W/initramfs-$a.cpio"
done
echo UPDATE-DONE
