#!/bin/sh
# vm-update.sh -- bring a "Devuan Ultimate" machine's AOK tools up to the repository's, in place. In the
# machine, as its user:
#     wget -qO- https://raw.githubusercontent.com/emkey1/ish-AOK/working/tests/bench-utm/utm/vm-update.sh | sudo sh
# (a machine built after 2026-10-12 has it as `sudo aok-update`).
#
# From its first boot a machine's disk is its user's (a password, keys, packages), so a fix to the scripts
# cannot reach it as a new image. This fetches the scripts of /AOK/tools again, replaces one only if what
# arrived parses, and repeats what later images set outside /AOK/tools (the power key is ignored; Mesa's
# Vulkan drivers are installed).
# AOK_UPDATE_URL names another place to fetch from, laid out like the repository.
set -e
[ "$(id -u)" = 0 ] || { echo "aok-update: run it as root (sudo)"; exit 1; }
[ -d /proc/ish ] && { echo "aok-update: for a VM; an AOK root has /AOK/tools from the app"; exit 1; }
base=${AOK_UPDATE_URL:-https://raw.githubusercontent.com/emkey1/ish-AOK/working}
tools="start-wayland.sh setup-wayland.sh provision-ultimate-devuan.sh setup-games.sh aok-sdl-game"
t=$(mktemp -d)
trap 'rm -rf "${t:?}"' EXIT
for f in $tools vm-update.sh; do
    case $f in vm-update.sh) u=$base/tests/bench-utm/utm/$f ;; *) u=$base/opt/AOK/tools/$f ;; esac
    if ! wget -q -O "$t/$f" "$u" || ! [ -s "$t/$f" ] || ! sh -n "$t/$f" 2> /dev/null; then
        echo "aok-update: could not fetch a sound $f from $base; nothing was changed"
        exit 1
    fi
done
mkdir -p /AOK/tools
put() {     # put FILE DEST: replace DEST if FILE differs
    if cmp -s "$1" "$2"; then
        echo "  $2: current"
    else
        install -m 755 "$1" "$2" && echo "  $2: updated"
    fi
}
for f in $tools; do put "$t/$f" "/AOK/tools/$f"; done
newer=; cmp -s "$t/vm-update.sh" /usr/local/bin/aok-update || newer=1
put "$t/vm-update.sh" /usr/local/bin/aok-update
# Run as the installed command and replaced by a newer one: the rest is the newer one's to do, or what it
# added would wait for a second run.
if [ -n "$newer" ] && [ "${0##*/}" = aok-update ] && [ -z "$AOK_UPDATE_AGAIN" ]; then
    echo "  (aok-update itself was updated: going on with the new one)"
    rm -rf "${t:?}"; trap - EXIT
    AOK_UPDATE_AGAIN=1 exec sh /usr/local/bin/aok-update
fi
# An iPad keyboard's power key reaches the guest as one, and elogind's default is to power off.
k=/etc/elogind/logind.conf.d/50-aok-vm-keys.conf
if [ -d /etc/elogind ] && ! [ -f "$k" ]; then
    mkdir -p /etc/elogind/logind.conf.d
    cat > "$k" <<'L'
[Login]
HandlePowerKey=ignore
HandleSuspendKey=ignore
HandleHibernateKey=ignore
L
    echo "  $k: written (in force from the next boot)"
fi
# Which engine runs the machine (2026-10-13: the AOK gadget engine says so; `aok-engine`, and a line at login).
cat > "$t/aok-engine" <<'E'
#!/bin/sh
# aok-engine -- which engine runs this machine's CPU. The AOK gadget engine says so itself: in the device
# tree (/chosen, "aok,engine") where the machine has one, and in a fw_cfg file that rc.local copies to
# /run/aok-engine at boot. Neither is there under QEMU's own engine.
if [ -d /proc/ish ]; then echo "iSH-AOK"; exit 0; fi
e=$(tr -d '\000' < /proc/device-tree/chosen/aok,engine 2> /dev/null)
[ -n "$e" ] || e=$(cat /run/aok-engine 2> /dev/null)
echo "${e:-QEMU, without the AOK gadgets}"
E
put "$t/aok-engine" /usr/local/bin/aok-engine
cat > "$t/aok-engine.sh" <<'E'
# (aok-engine) one line at login: which engine this machine runs on
case $- in *i*) command -v aok-engine > /dev/null 2>&1 && printf '  engine: \033[1m%s\033[0m\n' "$(aok-engine)" ;; esac
E
if ! cmp -s "$t/aok-engine.sh" /etc/profile.d/aok-engine.sh; then
    install -m 644 "$t/aok-engine.sh" /etc/profile.d/aok-engine.sh && echo "  /etc/profile.d/aok-engine.sh: written"
fi
if [ -f /etc/rc.local ] && ! grep -q "(aok-engine)" /etc/rc.local; then
    cat > "$t/rc-hook" <<'E'
# (aok-engine) the engine's fw_cfg file, for machines with no device tree
modprobe -q qemu_fw_cfg 2> /dev/null
cat /sys/firmware/qemu_fw_cfg/by_name/opt/aok/engine/raw > /run/aok-engine 2> /dev/null || rm -f /run/aok-engine
E
    # (after the #! line: rc.local ends in "exit 0")
    { sed -n 1p /etc/rc.local; cat "$t/rc-hook"; sed 1d /etc/rc.local; } > "$t/rc.local" &&
        install -m 755 "$t/rc.local" /etc/rc.local && echo "  /etc/rc.local: reads the engine's name at boot"
fi
echo "  this machine runs on: $(/usr/local/bin/aok-engine)"
# Vulkan: the engine offers it since 2026-10-13 (venus); the guest's half is Mesa's virtio driver, which
# images before that did not carry.
if ! dpkg -s mesa-vulkan-drivers vulkan-tools > /dev/null 2>&1; then
    echo "  mesa-vulkan-drivers, vulkan-tools: installing (Vulkan in the guest) ..."
    export DEBIAN_FRONTEND=noninteractive
    if apt-get update -qq > /dev/null 2>&1 &&
       apt-get install -y -qq --no-install-recommends mesa-vulkan-drivers vulkan-tools > /dev/null 2>&1; then
        echo "  mesa-vulkan-drivers, vulkan-tools: installed"
    else
        echo "  mesa-vulkan-drivers, vulkan-tools: NOT installed (apt failed; run aok-update again)"
    fi
fi
echo "aok-update: done. A desktop that is running keeps its old script until it is started again."
