#!/bin/sh
# digest.sh <label> [root] -- on the device: the digest tools (coreutils' or
# busybox's, whichever the root has) over 48 MB, milliseconds each, best of
# two; one line of results on stdout. In the booted root, or in the one named,
# entered with mount-root.sh as bench.sh does. python3 does the timing, so
# that it is the same clock in every root. digestab.sh puts this on the
# device and runs it.
if grep -q "^low_power_mode: Enabled" /proc/ish/BAT0 2>/dev/null; then
    echo "REFUSED: Low Power Mode is on"
    exit 3
fi
label=$1 root=${2:-booted}
if [ "$root" = booted ]; then run() { sh -c "$1"; }; else run() { sudo -n /AOK/tools/mount-root.sh "$root" -- sh -c "$1"; }; fi
out=$(run 'python3 -c "
import os, shutil, subprocess, time
blob = \"/tmp/digest-ab.blob\"
if not os.path.exists(blob) or os.path.getsize(blob) != 48000000:
    open(blob, \"wb\").write(bytes(48000000))
open(blob, \"rb\").read()
out = []
for d in (\"md5sum\", \"sha1sum\", \"sha256sum\", \"sha512sum\", \"b2sum\", \"cksum\"):
    if not shutil.which(d):
        continue
    best = None
    for i in range(2):
        t = time.monotonic()
        subprocess.run([d, blob], stdout=subprocess.DEVNULL)
        t = time.monotonic() - t
        best = t if best is None or t < best else best
    out.append(\"%s=%d\" % (d.replace(\"sum\", \"\"), best * 1000))
print(\" \".join(out))
"' < /dev/null 2>&1 | tail -1)
echo "$label $root $out"
