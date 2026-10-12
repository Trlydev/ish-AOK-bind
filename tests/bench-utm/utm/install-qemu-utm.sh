#!/bin/bash
# install-qemu-utm.sh [ios-tci|ios_simulator-tci] [stock] -- put the AOK-gadgets QEMU into UTM's sysroot as
# its RISC-V engine, or put UTM's own back.
#
# The first run keeps UTM's framework and RISC-V firmware under utm/stock-PLATFORM. Then the library that
# build-qemu-utm.sh installed is wrapped as qemu-riscv64-softmmu.framework by UTM's own scripts/fixup.sh
# (which rewrites its imports to the sysroot's frameworks), and the RISC-V firmware of the same QEMU
# (OpenSBI, EDK2) replaces the sysroot's. Every other architecture's QEMU is left as UTM built it.
set -e
W=${W:-$HOME/aok-vm-bench}
U=$W/utm
PLAT=ios-tci
case $1 in ios-tci|ios_simulator-tci) PLAT=$1; shift ;; esac
PREFIX=$U/UTM/sysroot-$PLAT-arm64
STOCK=$U/stock-$PLAT
OUT=$U/qemu-out-$PLAT
FW=$PREFIX/Frameworks/qemu-riscv64-softmmu.framework
FILES="opensbi-riscv64-generic-fw_dynamic.bin edk2-riscv-code.fd edk2-riscv-vars.fd"

if [ ! -d "$STOCK/qemu-riscv64-softmmu.framework" ]; then
    mkdir -p "$STOCK"
    cp -a "$FW" "$STOCK/"
    for f in $FILES; do cp -p "$PREFIX/share/qemu/$f" "$STOCK/"; done
fi
if [ "$1" = stock ]; then
    rsync -a --delete "$STOCK/qemu-riscv64-softmmu.framework/" "$FW/"
    for f in $FILES; do cp -p "$STOCK/$f" "$PREFIX/share/qemu/"; done
    echo "sysroot: UTM's own RISC-V QEMU"
    exit 0
fi
"$U/UTM/scripts/fixup.sh" -p "$PLAT" -s "$PREFIX" -m 15.0 "$OUT/lib/libqemu-riscv64-softmmu.dylib"
for f in $FILES; do cp -p "$OUT/share/qemu/$f" "$PREFIX/share/qemu/"; done
otool -L "$FW/qemu-riscv64-softmmu" | tail -n +2 | awk '{print "  " $1}'
echo "sysroot: AOK-gadgets RISC-V QEMU ($(nm "$FW/qemu-riscv64-softmmu" | grep -c aok_gadget_) gadgets)"
