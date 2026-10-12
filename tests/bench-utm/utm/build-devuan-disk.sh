#!/bin/bash
# build-devuan-disk.sh [SIZE] -- build the Devuan RISC-V machine for the VM front end: a disk image, and the
# kernel and initrd that boot it. Run on the build Mac, in ~/aok-vm-bench.
#
# No container and no cross-anything: the Alpine RISC-V guest of this directory (kernel/Image-lts-riscv64 and
# initramfs-riscv64-vm.cpio) is booted on Homebrew's qemu-system-riscv64, which has a JIT and a network, with
# three disks -- the new one, devuan-provision.sh as a tar, and one to hand things back -- and its /init runs
# the script (script=vdb). debootstrap and every package script then run natively.
#
# Twice: the first pass installs the base system and lists the packages to add, which fetch-debs.py then
# fetches on the host with many connections (the mirrors are slow per connection here, and the list is
# 300 MB) and a web server on the host hands to the second pass. An existing devuan.img is continued, not
# rebuilt: remove it to start over.
# Out: devuan/devuan-riscv64.qcow2, devuan/vmlinuz, devuan/initrd.img.
set -e
W=${W:-$HOME/aok-vm-bench}
here=$(cd "$(dirname "$0")" && pwd)
export PATH=/opt/homebrew/bin:$PATH
D=$W/devuan; mkdir -p "$D"; cd "$D"
[ -f devuan.img ] || qemu-img create -q -f raw devuan.img "${1:-6G}"
pass() {    # pass MODE: one run of the build VM
    rm -rf job job.tar bootout.img && mkdir job
    cp "$here/devuan-provision.sh" job/run.sh && echo "$1" > job/mode
    [ -f uris.txt ] && cp uris.txt job/
    tar -cf job.tar -C job . && truncate -s 2M job.tar
    qemu-img create -q -f raw bootout.img 256M
    qemu-system-riscv64 -M virt -smp 8 -m 4096 -display none -serial stdio -monitor none -no-reboot \
        -kernel "$W/kernel/Image-lts-riscv64" -initrd "$W/initramfs-riscv64-vm.cpio" \
        -append "console=ttyS0 rdinit=/init quiet bench=none script=vdb" \
        -drive file=devuan.img,format=raw,if=none,id=d0 -device virtio-blk-pci,drive=d0 \
        -drive file=job.tar,format=raw,if=none,id=d1,readonly=on -device virtio-blk-pci,drive=d1 \
        -drive file=bootout.img,format=raw,if=none,id=d2 -device virtio-blk-pci,drive=d2 \
        -netdev user,id=n0 -device virtio-net-pci,netdev=n0 -device virtio-rng-pci < /dev/null > "build-$1.log" 2>&1
}
pass list
grep -q DEVUAN-LIST-OK build-list.log || { tail -25 build-list.log; exit 1; }
tar -xf bootout.img uris.txt
python3 "$here/fetch-debs.py" uris.txt debs
python3 -m http.server 8642 --bind 127.0.0.1 --directory debs > /dev/null 2>&1 &
server=$!; trap 'kill $server 2>/dev/null' EXIT
pass install
grep -q DEVUAN-PROVISION-OK build-install.log || { tail -25 build-install.log; exit 1; }
rm -rf boot && mkdir boot && tar -xf bootout.img -C boot
cp boot/vmlinu* vmlinuz && cp boot/initrd.img* initrd.img
qemu-img convert -O qcow2 -c devuan.img devuan-riscv64.qcow2
ls -la devuan-riscv64.qcow2 vmlinuz initrd.img
