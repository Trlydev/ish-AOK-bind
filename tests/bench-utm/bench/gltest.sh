#!/bin/sh
# gltest.sh [FRAMES] -- is the display accelerated? Installs Mesa (the guest has none: about 250 MB into RAM,
# so give the VM 2 GB) and runs kmscube on the console's display, which prints the GL renderer it was given:
# "virgl (...)" when the host renders, "llvmpipe" when the guest's CPU does. Needs the network.
apk add --no-cache mesa-dri-gallium mesa-gbm mesa-egl mesa-gles kmscube > /tmp/gltest-apk.log 2>&1 ||
    { echo "GLTEST apk failed:"; tail -3 /tmp/gltest-apk.log; exit 1; }
kmscube -c "${1:-600}" 2>&1 | grep -iE "renderer|version|vendor|Rendered|error|fail" | sed 's/^/GLTEST /'
