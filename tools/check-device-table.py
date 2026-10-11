#!/usr/bin/env python3
"""List the iPhones, iPads and iPods Xcode knows that kernel/hostinfo.m does not.

/proc/cpuinfo's "host device" line and /proc/ish/host_info come from the tables
in kernel/hostinfo.m; a model missing there shows as its bare identifier
(iPhone19,2). Xcode ships a database of every device it can build for, so run
this after each Xcode update, and before a release:

    tools/check-device-table.py

Prints one line per missing model -- identifier, Xcode's name, its chip
platform -- and exits 1 if there are any. 32-bit devices are left out.
"""
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    dev = subprocess.run(['xcode-select', '-p'], capture_output=True, text=True).stdout.strip()
    db = os.path.join(dev, 'Platforms/iPhoneOS.platform/usr/standalone/device_traits.db')
    if not os.path.exists(db):
        print('no device database at %s' % db, file=sys.stderr)
        return 2
    rows = subprocess.run(
        ['sqlite3', '-separator', '|', db,
         "SELECT DISTINCT ProductType, ProductDescription, Platform FROM Devices "
         "WHERE ProductType LIKE 'iPhone%' OR ProductType LIKE 'iPad%' "
         "OR ProductType LIKE 'iPod%' ORDER BY ProductType;"],
        capture_output=True, text=True, check=True).stdout.splitlines()
    source = open(os.path.join(REPO, 'kernel/hostinfo.m')).read()
    have = set(re.findall(r'@"((?:iPhone|iPad|iPod)\d+,\d+)"', source))
    missing = {}
    for row in rows:
        ident, name, platform = row.split('|')
        ident = ident.split('-')[0]         # iPad16,3-A and -B are one model
        if platform.startswith('s5l'):      # 32-bit: AOK does not run there
            continue
        if ident not in have:
            missing[ident] = (name, platform)
    # Xcode lists some models before they have a name ("iPhone"): nothing to
    # put in the table yet, so they are mentioned and do not fail the check.
    unnamed = {i: m for i, m in missing.items() if m[0] in ('iPhone', 'iPad', 'iPod')}
    for ident in unnamed:
        del missing[ident]
    order = lambda i: [int(n) if n.isdigit() else n for n in re.split(r'(\d+)', i)]
    for ident in sorted(missing, key=order):
        print('%-12s %-40s %s' % (ident, *missing[ident]))
    for ident in sorted(unnamed, key=order):
        print('%-12s not named by this Xcode yet (%s)' % (ident, unnamed[ident][1]))
    if not missing:
        print('kernel/hostinfo.m has every model this Xcode names (%d)' % len(have))
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
