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
    check)    ./check.sh; ./timeit check true ;;
    stress)   # four transcripts at once, for a guest with several CPUs: all four must be the one check prints
              for i in 1 2 3 4; do ./check.sh > /tmp/check.$i 2>&1 & done; wait
              for i in 1 2 3 4; do echo "STRESS $i $(md5sum < /tmp/check.$i | cut -c1-32)"; done
              ./timeit stress true ;;
    fptest)   ./fptest; ./timeit fptest true ;;
    memtest)  ./memtest; ./timeit memtest true ;;
    fptestv)  ./fptest -v; ./timeit fptestv true ;;
    simdtest) ./simdtest; ./timeit simdtest true ;;         # aarch64: every FP/SIMD encoding
    simdtest-*) ./simdtest -b "${w#simdtest-}"; ./timeit "$w" true ;;   # one bucket of it, a line an encoding
    *)        ./timeit "$w" true ;;
    esac
done
