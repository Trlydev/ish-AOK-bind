// AArch64 guest integer gadgets fed precomputed cpu_state offsets, the
// scalar side of simd_spec.S: gen.c resolves each register (SP included) to
// its byte offset in cpu_state, so the gadget is the operation and nothing
// else. Switched by the "ospec" bit of /proc/ish/arm64_jit_fuse.
//
// Each operand is its own stream word, fetched with ldp/ldr: packing them
// into one word and unpacking with ubfx put the shifts on the critical path
// and measured 29% slower per instruction on an A10X (26% on an M4) than
// the plain loads, which the core issues in parallel.
//
// Code streams: immediate forms [d_off][n_off][imm]; register forms
// [d_off][n_off][m_off] (+[shift] for the _lsl ones); CMP/CMN drop d_off.
// A 32-bit form stores the whole 64-bit slot: the W-register op zeroes the
// upper half.

#include "gadgets.h"
#include "frag.h"

.text

.macro ospec_alu_imm name, op, r, flags=0
    f_gadget \name, f_alu_imm, \op, \r, \flags, _, _
.endm
ospec_alu_imm ospec_addi64, add, x
ospec_alu_imm ospec_addi32, add, w
ospec_alu_imm ospec_subi64, sub, x
ospec_alu_imm ospec_subi32, sub, w
ospec_alu_imm ospec_addsi64, adds, x, 1
ospec_alu_imm ospec_addsi32, adds, w, 1
ospec_alu_imm ospec_subsi64, subs, x, 1
ospec_alu_imm ospec_subsi32, subs, w, 1
// Logical (immediate): the decoded bitmask is the third word.
ospec_alu_imm ospec_andi64, and, x
ospec_alu_imm ospec_andi32, and, w
ospec_alu_imm ospec_orri64, orr, x
ospec_alu_imm ospec_orri32, orr, w
ospec_alu_imm ospec_eori64, eor, x
ospec_alu_imm ospec_eori32, eor, w
ospec_alu_imm ospec_andsi64, ands, x, 1
ospec_alu_imm ospec_andsi32, ands, w, 1

// CMP/CMN (immediate): flags only. [n_off][imm]
.macro ospec_cmp_imm name, op, r
.gadget \name
    ldp x10, x11, [_ip]
    ldr x12, [_cpu, x10]
    \op \r\()zr, \r\()12, \r\()11
    store_flags
    gret 2
.endm
ospec_cmp_imm ospec_cmpi64, subs, x
ospec_cmp_imm ospec_cmpi32, subs, w
ospec_cmp_imm ospec_cmni64, adds, x
ospec_cmp_imm ospec_cmni32, adds, w

.macro ospec_alu_reg name, op, r, flags=0
    f_gadget \name, f_alu_reg, \op, \r, \flags, _, _
.endm
ospec_alu_reg ospec_addr64, add, x
ospec_alu_reg ospec_addr32, add, w
ospec_alu_reg ospec_subr64, sub, x
ospec_alu_reg ospec_subr32, sub, w
ospec_alu_reg ospec_andr64, and, x
ospec_alu_reg ospec_andr32, and, w
ospec_alu_reg ospec_orrr64, orr, x
ospec_alu_reg ospec_orrr32, orr, w
ospec_alu_reg ospec_eorr64, eor, x
ospec_alu_reg ospec_eorr32, eor, w
ospec_alu_reg ospec_addsr64, adds, x, 1
ospec_alu_reg ospec_addsr32, adds, w, 1
ospec_alu_reg ospec_subsr64, subs, x, 1
ospec_alu_reg ospec_subsr32, subs, w, 1
ospec_alu_reg ospec_andsr64, ands, x, 1
ospec_alu_reg ospec_andsr32, ands, w, 1

// ADD/SUB with Rm shifted left by a constant (address arithmetic).
// [d_off][n_off][m_off][shift]
.macro ospec_alu_reg_lsl name, op, r
    f_gadget \name, f_alu_reg_lsl, \op, \r, _, _, _
.endm
ospec_alu_reg_lsl ospec_addr_lsl64, add, x
ospec_alu_reg_lsl ospec_addr_lsl32, add, w
ospec_alu_reg_lsl ospec_subr_lsl64, sub, x
ospec_alu_reg_lsl ospec_subr_lsl32, sub, w

// CMP/CMN (register): [n_off][m_off]
.macro ospec_cmp_reg name, op, r
.gadget \name
    ldp x10, x11, [_ip]
    ldr x12, [_cpu, x10]
    ldr x13, [_cpu, x11]
    \op \r\()zr, \r\()12, \r\()13
    store_flags
    gret 2
.endm
ospec_cmp_reg ospec_cmpr64, subs, x
ospec_cmp_reg ospec_cmpr32, subs, w
ospec_cmp_reg ospec_cmnr64, adds, x
ospec_cmp_reg ospec_cmnr32, adds, w

// UBFM/SBFM with real registers: LSL/LSR/ASR by a constant, UBFX/SBFX,
// UBFIZ, the extends. [d_off][n_off][L][S], the generic gadget's two shift
// counts (dpextra.S) as their own words. The generic ubfm was 3% of the
// gadgets Python runs and 10% of 7-Zip's, and decoded five fields and took
// four branches to do two shifts.
f_gadget ospec_ubfm64, f_bfm, lsrv, x, _, _, _
f_gadget ospec_ubfm32, f_bfm, lsrv, w, _, _, _
f_gadget ospec_sbfm64, f_bfm, asrv, x, _, _, _
f_gadget ospec_sbfm32, f_bfm, asrv, w, _, _, _

// Add/subtract with carry and the multiplies, with real registers: what
// bignum arithmetic is made of (RSA and the key exchanges of ssh and TLS: 37%
// of the gadgets an `openssl genrsa` ran), and what the generic adcsbc and
// dp3src gadgets decoded six fields and took a branch tree to do.
.irp op, adc, adcs, sbc, sbcs
.ifc \op, adc
f_gadget ospec_\op\()64, f_adc, \op, x, 0, 0, _
f_gadget ospec_\op\()32, f_adc, \op, w, 0, 0, _
f_gadget ospec_\op\()64_nf, f_adc, \op, x, 0, 1, _
f_gadget ospec_\op\()32_nf, f_adc, \op, w, 0, 1, _
.endif
.ifc \op, sbc
f_gadget ospec_\op\()64, f_adc, \op, x, 0, 0, _
f_gadget ospec_\op\()32, f_adc, \op, w, 0, 0, _
f_gadget ospec_\op\()64_nf, f_adc, \op, x, 0, 1, _
f_gadget ospec_\op\()32_nf, f_adc, \op, w, 0, 1, _
.endif
.ifc \op, adcs
f_gadget ospec_\op\()64, f_adc, \op, x, 1, 0, _
f_gadget ospec_\op\()32, f_adc, \op, w, 1, 0, _
f_gadget ospec_\op\()64_nf, f_adc, \op, x, 1, 1, _
f_gadget ospec_\op\()32_nf, f_adc, \op, w, 1, 1, _
.endif
.ifc \op, sbcs
f_gadget ospec_\op\()64, f_adc, \op, x, 1, 0, _
f_gadget ospec_\op\()32, f_adc, \op, w, 1, 0, _
f_gadget ospec_\op\()64_nf, f_adc, \op, x, 1, 1, _
f_gadget ospec_\op\()32_nf, f_adc, \op, w, 1, 1, _
.endif
.endr
f_gadget ospec_mul64, f_alu_reg, mul, x, 0, _, _
f_gadget ospec_mul32, f_alu_reg, mul, w, 0, _, _
f_gadget ospec_umulh, f_alu_reg, umulh, x, 0, _, _
f_gadget ospec_smulh, f_alu_reg, smulh, x, 0, _, _
f_gadget ospec_madd64, f_madd, madd, x, _, _, _
f_gadget ospec_madd32, f_madd, madd, w, _, _, _
f_gadget ospec_msub64, f_madd, msub, x, _, _, _
f_gadget ospec_msub32, f_madd, msub, w, _, _, _
f_gadget ospec_umull, f_mull, umull, _, _, _, _
f_gadget ospec_smull, f_mull, smull, _, _, _, _
