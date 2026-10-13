#!/bin/bash
# install-qemu-utm.sh [ios-tci|ios_simulator-tci] [stock] -- put the AOK-gadgets QEMU into UTM's sysroot as
# its RISC-V and ARM64 engines, or put UTM's own back.
#
# The first run keeps UTM's frameworks and RISC-V firmware under utm/stock-PLATFORM. Then each library that
# build-qemu-utm.sh installed is wrapped as qemu-ARCH-softmmu.framework by UTM's own scripts/fixup.sh
# (which rewrites its imports to the sysroot's frameworks), and the RISC-V firmware of the same QEMU
# (OpenSBI, EDK2) replaces the sysroot's; ARM64's firmware stays UTM's. Every other architecture's QEMU is
# left as UTM built it.
set -e
W=${W:-$HOME/aok-vm-bench}
U=$W/utm
PLAT=ios-tci
case $1 in ios-tci|ios_simulator-tci) PLAT=$1; shift ;; esac
PREFIX=$U/UTM/sysroot-$PLAT-arm64
STOCK=$U/stock-$PLAT
OUT=$U/qemu-out-$PLAT${AOK_GL:+-gl}     # AOK_GL=1: the build with OpenGL and virglrenderer
ARCHS="riscv64 aarch64"
FILES="opensbi-riscv64-generic-fw_dynamic.bin edk2-riscv-code.fd edk2-riscv-vars.fd"

mkdir -p "$STOCK"
for a in $ARCHS; do
    [ -d "$STOCK/qemu-$a-softmmu.framework" ] || cp -a "$PREFIX/Frameworks/qemu-$a-softmmu.framework" "$STOCK/"
done
for f in $FILES; do [ -f "$STOCK/$f" ] || cp -p "$PREFIX/share/qemu/$f" "$STOCK/"; done
if [ "$1" = stock ]; then
    for a in $ARCHS; do
        rsync -a --delete "$STOCK/qemu-$a-softmmu.framework/" "$PREFIX/Frameworks/qemu-$a-softmmu.framework/"
    done
    for f in $FILES; do cp -p "$STOCK/$f" "$PREFIX/share/qemu/"; done
    echo "sysroot: UTM's own RISC-V and ARM64 QEMU"
    exit 0
fi
for a in $ARCHS; do
    "$U/UTM/scripts/fixup.sh" -p "$PLAT" -s "$PREFIX" -m 15.0 "$OUT/lib/libqemu-$a-softmmu.dylib"
done
for f in $FILES; do cp -p "$OUT/share/qemu/$f" "$PREFIX/share/qemu/"; done
for a in $ARCHS; do
    FW=$PREFIX/Frameworks/qemu-$a-softmmu.framework
    [ "$a" = riscv64 ] && otool -L "$FW/qemu-$a-softmmu" | tail -n +2 | awk '{print "  " $1}'
    echo "sysroot: AOK-gadgets $a QEMU ($(nm "$FW/qemu-$a-softmmu" | grep -c aok_gadget_) gadgets)"
done
