#!/bin/sh
# vm-update.sh -- bring a "Devuan Ultimate" machine's AOK tools up to the repository's, in place. In the
# machine, as its user:
#     wget -qO- https://raw.githubusercontent.com/emkey1/ish-AOK/working/tests/bench-utm/utm/vm-update.sh | sudo sh
# (a machine built after 2026-10-12 has it as `sudo aok-update`).
#
# From its first boot a machine's disk is its user's (a password, keys, packages), so a fix to the scripts
# cannot reach it as a new image. This fetches the scripts of /AOK/tools again, replaces one only if what
# arrived parses, and repeats what later images set outside /AOK/tools (so far: the power key is ignored).
# AOK_UPDATE_URL names another place to fetch from, laid out like the repository.
set -e
[ "$(id -u)" = 0 ] || { echo "aok-update: run it as root (sudo)"; exit 1; }
[ -d /proc/ish ] && { echo "aok-update: for a VM; an AOK root has /AOK/tools from the app"; exit 1; }
base=${AOK_UPDATE_URL:-https://raw.githubusercontent.com/emkey1/ish-AOK/working}
tools="start-wayland.sh setup-wayland.sh provision-ultimate-devuan.sh"
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
put "$t/vm-update.sh" /usr/local/bin/aok-update
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
echo "aok-update: done. A desktop that is running keeps its old script until it is started again."
