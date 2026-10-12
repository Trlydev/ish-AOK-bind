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
#
# build-devuan-disk.sh ultimate -- then make that machine the "ultimate" one: with iSH-AOK's own
# provisioning and Wayland scripts in /AOK/tools (copy opt/AOK/tools/provision-ultimate-devuan.sh,
# setup-wayland.sh, setup-wayland-extras.sh, start-wayland.sh and, as wayland/, build-release-guard.sh and
# ish_wl_release_guard.c of opt/AOK/tools/wayland to ~/aok-vm-bench/aok-tools first) and already run. They are run in the machine itself, booted with its own kernel on the same QEMU, by
# rc.local's aokjob= hook (devuan-provision.sh), exactly as its owner would run them; devuan-ultimate-job.sh
# is the job. An hour or so, most of it package scripts.
# build-devuan-disk.sh desktop-test -- boot it with a display, start the desktop, and leave a picture of
# it in devuan/desktop.png. QEMU_BIN names the QEMU to do that on (default: the one that built it), so
#     QEMU_BIN=~/aok-vm-bench/build-aok/qemu-system-riscv64 QEMU_AOK_STATS=1 build-devuan-disk.sh desktop-test
# is the desktop on the AOK-gadgets engine: build-desktop.log then ends with its "aok:" lines, and without
# them it was not that engine. (This script puts Homebrew first on PATH: a PATH prefix does not choose.)
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
QEMU_BIN=${QEMU_BIN:-qemu-system-riscv64}
devuan_boot() {   # devuan_boot LOG APPEND [qemu args...]: the machine itself, with its own kernel
    local log=$1 append=$2 net=; shift 2
    # (a QEMU built without the user-mode network: the machine boots without one, a minute slower)
    "$QEMU_BIN" -netdev help 2>&1 | grep -qx user && net="-netdev user,id=n0 -device virtio-net-pci,netdev=n0"
    "$QEMU_BIN" -M virt -smp 8 -m 4096 -display none -serial stdio -monitor none -no-reboot \
        -kernel vmlinuz -initrd initrd.img -append "root=/dev/vda rw console=ttyS0 net.ifnames=0 $append" \
        -drive file=devuan.img,format=raw,if=none,id=d0 -device virtio-blk-pci,drive=d0 \
        $net -device virtio-rng-pci "$@" < /dev/null > "$log" 2>&1
}
job_tar() {       # job_tar SCRIPT: job.tar with it as run.sh
    rm -rf job job.tar && mkdir job && cp "$1" job/run.sh
    tar -cf job.tar -C job . && truncate -s 2M job.tar
}
if [ "$1" = ultimate ]; then
    [ -f devuan.img ] && [ -d "$W/aok-tools" ] || { echo "needs devuan/devuan.img and $W/aok-tools"; exit 1; }
    rm -rf job job.tar bootout.img && mkdir -p job/tools
    cp "$here/devuan-provision.sh" job/run.sh && echo tools > job/mode
    for f in "$W"/aok-tools/*; do
        case $f in */provision-ultimate-alpine.sh) ;; *) cp -r "$f" job/tools/ ;; esac
    done
    tar -cf job.tar -C job . && truncate -s 2M job.tar
    qemu-img create -q -f raw bootout.img 16M
    qemu-system-riscv64 -M virt -smp 8 -m 4096 -display none -serial stdio -monitor none -no-reboot \
        -kernel "$W/kernel/Image-lts-riscv64" -initrd "$W/initramfs-riscv64-vm.cpio" \
        -append "console=ttyS0 rdinit=/init quiet bench=none script=vdb" \
        -drive file=devuan.img,format=raw,if=none,id=d0 -device virtio-blk-pci,drive=d0 \
        -drive file=job.tar,format=raw,if=none,id=d1,readonly=on -device virtio-blk-pci,drive=d1 \
        -drive file=bootout.img,format=raw,if=none,id=d2 -device virtio-blk-pci,drive=d2 \
        -netdev user,id=n0 -device virtio-net-pci,netdev=n0 -device virtio-rng-pci < /dev/null > build-tools.log 2>&1
    grep -q DEVUAN-TOOLS-OK build-tools.log || { tail -25 build-tools.log; exit 1; }
    job_tar "$here/devuan-ultimate-job.sh"
    devuan_boot build-ultimate.log "aokjob=vdb" \
        -drive file=job.tar,format=raw,if=none,id=d1,readonly=on -device virtio-blk-pci,drive=d1
    grep -q "AOKJOB-DONE rc=0" build-ultimate.log || { tail -40 build-ultimate.log; exit 1; }
    qemu-img convert -O qcow2 -c devuan.img devuan-riscv64.qcow2
    ls -la devuan-riscv64.qcow2
    exit 0
fi
if [ "$1" = desktop-test ]; then
    # The desktop as the login that owns it starts it, on a display nobody watches; the picture is taken
    # through QEMU's monitor a while after, and the job then powers the machine off.
    cat > desktop-job.sh <<'J'
#!/bin/sh
u=$(awk -F: '$3 >= 1000 && $3 < 2000 { print $1; exit }' /etc/passwd)
echo "DESKTOP-TEST as ${u:-root}; cards: $(ls /dev/dri 2>/dev/null | tr '\n' ' ')"
if [ -n "$u" ]; then su - "$u" -c 'sh /AOK/tools/start-wayland.sh' &
else sh /AOK/tools/start-wayland.sh & fi
sleep 150
echo "DESKTOP-TEST processes: $(ps -e -o comm= | grep -E 'labwc|foot|waybar|seatd|wayvnc' | sort | uniq -c | tr '\n' ' ')"
echo "DESKTOP-TEST power key: $(grep -c 'Power key pressed' /var/log/auth.log) pressed, $(grep -c 'Powering off' /var/log/auth.log) obeyed (auth.log), and still up"
J
    job_tar desktop-job.sh
    rm -f mon.sock desktop.ppm
    devuan_boot build-desktop.log "aokjob=vdb" \
        -drive file=job.tar,format=raw,if=none,id=d1,readonly=on -device virtio-blk-pci,drive=d1 \
        -device virtio-gpu-pci -device virtio-keyboard-pci -device virtio-tablet-pci \
        -monitor unix:mon.sock,server,nowait -snapshot &
    qemu=$!
    for i in $(seq 1 1500); do grep -aq "DESKTOP-TEST as" build-desktop.log 2>/dev/null && break; sleep 1; done
    sleep 100
    # The power key, four times: the machine must note it and stay up (the job's last line says so).
    for i in 1 2 3 4; do echo "sendkey power" | nc -U mon.sock > /dev/null 2>&1 || true; sleep 1; done
    sleep 16
    echo "screendump $PWD/desktop.ppm" | nc -U mon.sock > /dev/null 2>&1 || true
    wait $qemu || true
    grep -a "DESKTOP-TEST\|AOKJOB-DONE\|^aok: " build-desktop.log
    sips -s format png desktop.ppm --out desktop.png > /dev/null 2>&1 || python3 - <<'PY'
import struct, zlib
d = open('desktop.ppm', 'rb').read()
p = d.split(b'\n', 3); w, h = map(int, p[1].split()); px = p[3]
raw = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
open('desktop.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                               chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
PY
    ls -la desktop.png
    exit 0
fi
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
