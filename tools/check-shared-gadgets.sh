#!/bin/sh
# check-shared-gadgets.sh -- do the riscv64 gadget sources still keep the
# contract a second host builds them under?
#
# jit/guest-riscv64/alu.S, frag.h and pairs.h are also the QEMU plug-in's
# source (frag.h, "A second host"). This assembles alu.S the way a second
# host does, with stand-in definitions of everything the contract makes the
# host's (registers deliberately not AOK's, a memory model and a way out of
# a block of its own), and fails if the shared code
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
_cpu  .req x20
_ip   .req x28
_addr .req x5
_sav  .req x6
_savw .req w6
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
.macro gret_ahead
    ldr x9, [_ip, #-8]
    br x9
.endm
// Memory: a host whose guest addresses are host addresses plus a word of
// its state, and whose slow path is its own code.
.macro read_prep size, id, ahead=0
    ldr x9, [_cpu]
    tbnz _addr, #63, handle_miss_\id
    add _addr, _addr, x9
back_\id:
.endm
.macro write_prep size, id, ahead=0
    read_prep \size, \id, \ahead
.endm
.macro write_done size, id
.endm
.macro read_bullshit size, id, ahead=0
handle_miss_\id:
    ldr x9, [_ip, #-(\ahead + 8)]
    add _addr, _cpu, x9
    b back_\id
.endm
.macro write_bullshit size, id, ahead=0
    read_bullshit \size, \id, \ahead
.endm
// Ways out: two words each, and code addresses relative to one in x21.
#define F_WAY_WORDS 2
.macro branch_leave off
    ldp x9, x10, [_ip, #\off]
    add _ip, x10, x9
    gret
.endm
.macro pc_param reg
    add \reg, x21, \reg
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
f_gadget b
.irp n, lb, lbu, lh, lhu, lw, lwu, ld, sb, sh, sw, sd
f_gadget \\n
.endr
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
# x0, x9-x15 and the registers the host named (x5, x6, x20, x28, and x21 inside
# its own pc_param); anything else is not the shared code's to use
bad=$(objdump -d --no-show-raw-insn "$tmp/host.o" |
      grep -oE '\<([xw](1[6-9]|2[2-79]|30|[1-478])|sp)\>' | sort -u | tr '\n' ' ')
[ -z "$bad" ] || { echo "shared gadget code uses registers outside the contract: $bad"; exit 1; }
echo "shared riscv64 gadgets: $(echo $have | wc -w | tr -d ' ') gadgets and" \
     "$(nm "$tmp/host.o" | grep -c 'shared_gadget_f2_') pairs keep the contract"
