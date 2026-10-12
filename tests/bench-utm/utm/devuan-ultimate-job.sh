#!/bin/sh
# devuan-ultimate-job.sh -- run inside the Devuan machine by rc.local's aokjob= hook, as root, once it has
# booted (build-devuan-disk.sh ultimate): iSH-AOK's own provisioning and Wayland scripts, from /AOK/tools,
# answering their questions through the environment. What it leaves:
#   - the "ultimate terminal" tool set and services of provision-ultimate-devuan.sh
#   - a login, aok, with no password and sudo without one; it is logged in on the display's console, root
#     still on the serial one (a local test machine: give both passwords before putting it on a network)
#   - labwc, foot, waybar and the rest of setup-wayland.sh, and seatd; `desktop` starts the desktop
set -x
export DEBIAN_FRONTEND=noninteractive LC_ALL=C
# The clock: a machine started with no network time yet can be far enough off for apt to refuse the
# archive's signatures.
date
export TZ_NAME=UTC TARGET_USER=aok NEW_HOSTNAME=devuan SUDO_NOPASSWD=1
sh /AOK/tools/provision-ultimate-devuan.sh || exit 1
sh /AOK/tools/setup-wayland.sh || exit 2
# X11 programs on the desktop, and a browser and file manager for its menu: best-effort.
sh /AOK/tools/setup-wayland-extras.sh x11 tools || echo "wayland extras: not all installed"
id aok
# aok on the display's console; root stays on the serial port.
sed -i 's|^1:2345:respawn:.*|1:2345:respawn:/sbin/agetty --autologin aok --noclear tty1 linux|' /etc/inittab
cat > /etc/motd <<'M'

   Devuan GNU/Linux 6 (excalibur), riscv64, for the AOK QEMU engine
   ------------------------------------------------------------
   desktop  :  desktop                  (Wayland: labwc, foot, waybar)
   display  :  gltest                   which GL renderer programs get
   re-setup :  sudo sh /AOK/tools/provision-ultimate-devuan.sh
               (timezone, login, hostname: it asks)
   services :  service <name> {start|stop|status}
   ------------------------------------------------------------

M
apt-get clean
rm -rf /var/lib/apt/lists/*
df -h / | tail -1
dpkg-query -W -f '${Package} ${Version}\n' labwc foot waybar seatd neovim gcc 2>&1
