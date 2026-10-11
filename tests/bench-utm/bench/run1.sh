#!/bin/sh
# run1.sh NAME... -- run the named workloads once each, in order. Same script under AOK and in the QEMU guests.
# Each is bracketed by a WL-START line and timeit's RESULT line, which the host-side cycwrap watches for.
cd /bench
for w in "$@"; do
    echo "WL-START $w"
    case "$w" in
    coremark) ./timeit coremark ./coremark 0x0 0x0 0x66 4000 7 1 2000 ;;
    nbody)    ./timeit nbody ./nbody 2000000 ;;
    python)   ./timeit python python3 pybench.py 1 ;;
    xz)       ./timeit xz xz -T1 -3 -c data2.bin ;;
    sha256)   ./timeit sha256 sh -c "sha256sum data.bin; sha256sum data.bin" ;;
    awk)      ./timeit awk awk '{ for (i = 1; i <= NF; i++) c[$i]++ } END { n = 0; for (w in c) n += c[w] * length(w); print n }' data1.bin ;;
    *)        ./timeit "$w" true ;;
    esac
done
