#!/bin/sh
# check-shared-gadgets.sh -- do the riscv64 gadget sources still keep the
# contract a second host builds them under?
#
# jit/guest-riscv64/alu.S, frag.h and pairs.h are also the QEMU plug-in's
# source (frag.h, "A second host"). This assembles alu.S the way a second
# host does, with stand-in definitions and deliberately different registers
# for _cpu and _ip, and fails if the shared code
#   - does not assemble without gadgets.h (it used something of AOK's own),
#   - refers to a symbol outside itself (it calls out or reads AOK's state),
#   - touches a host register the contract does not give it.
# Run it after editing those files. It needs nothing but the C compiler.
# With a directory, it checks the copies of the three files there instead.
set -e
src=$(cd "${1:-$(dirname "$0")/../jit/guest-riscv64}" && pwd)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT

# The second host's file, on stdout, with HOST_<gadget> 1 for the names
# in $have (none on the first pass).
probe() {
    cat <<'EOF'
#define RV_GADGET_HOST check
_cpu .req x20
_ip  .req x28
.macro .gadget name
    .balign 16
    .globl shared_gadget_\name
shared_gadget_\name:
.endm
.macro gret pop=0
    ldr x9, [_ip, #(\pop * 8)]
    add _ip, _ip, #(\pop * 8 + 8)
    br x9
.endm
EOF
    for n in $(sed -n 's/^#define F_\([A-Za-z0-9_]*\) 1$/\1/p' "$src/pairs.h"); do
        case " $have " in
        *" $n "*) echo "#define HOST_$n 1" ;;
        *) echo "#define HOST_$n 0" ;;
        esac
    done
    cat <<EOF
    .text
#include "$src/alu.S"
f_gadget mov_const
f_gadget addi
EOF
}

# first without pairs, to learn which gadgets a second host gets
have=
probe > "$tmp/probe.S"
cc -arch arm64 -c -o "$tmp/probe.o" "$tmp/probe.S"
have=$(nm "$tmp/probe.o" | sed -n 's/.* T _*shared_gadget_//p' | tr '\n' ' ')
# then with every pair both of whose gadgets it has
probe > "$tmp/host.S"
cc -arch arm64 -c -o "$tmp/host.o" "$tmp/host.S"

undefined=$(nm -u "$tmp/host.o")
[ -z "$undefined" ] || { echo "shared gadget code refers outside itself:"; echo "$undefined"; exit 1; }
# x0, x9-x15 and the two the host named; anything else is not the shared code's to use
bad=$(objdump -d --no-show-raw-insn "$tmp/host.o" |
      grep -oE '\<([xw](1[6-9]|2[1-79]|30|[1-8])|sp)\>' | sort -u | tr '\n' ' ')
[ -z "$bad" ] || { echo "shared gadget code uses registers outside the contract: $bad"; exit 1; }
echo "shared riscv64 gadgets: $(echo $have | wc -w | tr -d ' ') gadgets and" \
     "$(nm "$tmp/host.o" | grep -c 'shared_gadget_f2_') pairs keep the contract"
