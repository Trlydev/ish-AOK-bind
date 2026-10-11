// Gadget bodies as macros, so that one body serves the gadget it always was
// and the fused pairs of pairs.h (alu_spec.S's "Fused pairs").
//
// Under the gadget JIT a guest instruction costs a dispatch -- the load of
// the next gadget's address, the indirect branch, and on the hosts measured
// the two taken branches that bound how fast gadgets can follow each other --
// plus a few host instructions of real work. Two guest instructions that
// always run one after the other can share one dispatch: the pair gadget is
// the first body, then the second, then the dispatch. Nothing else changes.
// Each body still reads its own parameters from the stream and commits its
// result to cpu_state before the next begins, so a fault or a signal in the
// second finds the first already done, as it would between two gadgets.
//
// A family FOO is two macros:
//
//     FOO_body id, last, a1, a2, a3, a4, a5
//     FOO_stub id, last, a1, a2, a3, a4, a5
//
// The body is entered with _ip at its first parameter. With `last` 0 it
// leaves _ip at the word after its parameters and falls through; with `last`
// 1 it ends in the dispatch. The stub is the body's out-of-line code (TLB
// miss, page crossing), placed after everything that falls through; `id`
// makes its labels unique. a1..a5 are the family's own arguments; unused
// ones are passed as `_`.
//
// The standalone gadgets are defined from the same bodies with `last` 1
// (alu_spec.S, memory.S, simd_spec.S, math.S), so a pair cannot drift from
// the instructions it is made of.

// A body that cannot fault: one update of _ip, in the dispatch when it is last.
.macro f_end last, n
.if \last
    gret \n
.else
    add _ip, _ip, #(\n * 8)
.endif
.endm

// A body that can fault moves _ip before its access, past its own
// parameters (four, or `n`; the fault paths find the instruction's address in
// the last of them) -- and, when it is the last body, past the next gadget's word in the
// same move, dispatching through gret_ahead (gadgets.h).
.macro f_ip_past last, n=4
.if \last
    add _ip, _ip, #(\n * 8 + 8)
.else
    add _ip, _ip, #(\n * 8)
.endif
.endm
.macro f_end_ahead last
.if \last
    gret_ahead
.endif
.endm
.macro f_read_prep last, bits, id
.if \last
    read_prep \bits, \id, 8
.else
    read_prep \bits, \id
.endif
.endm
.macro f_write_prep last, bits, id
.if \last
    write_prep \bits, \id, 8
.else
    write_prep \bits, \id
.endif
.endm
.macro f_read_bullshit last, bits, id
.if \last
    read_bullshit \bits, \id, 8
.else
    read_bullshit \bits, \id
.endif
.endm
.macro f_write_bullshit last, bits, id
.if \last
    write_bullshit \bits, \id, 8
.else
    write_bullshit \bits, \id
.endif
.endm

// ---- Integer ALU, precomputed slots (alu_spec.S) -------------------------

// [d_off][n_off][imm]: op, r, flags
.macro f_alu_imm_body id, last, op, r, flags, a4, a5
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr x12, [_cpu, x10]
    \op \r\()12, \r\()12, \r\()11
.if \flags
    store_flags
.endif
    str x12, [_cpu, x9]
    f_end \last, 3
.endm
.macro f_alu_imm_stub id, last, op, r, flags, a4, a5
.endm

// [d_off][n_off][m_off]: op, r, flags
.macro f_alu_reg_body id, last, op, r, flags, a4, a5
    ldp x9, x10, [_ip]
    ldr x11, [_ip, #16]
    ldr x12, [_cpu, x10]
    ldr x13, [_cpu, x11]
    \op \r\()12, \r\()12, \r\()13
.if \flags
    store_flags
.endif
    str x12, [_cpu, x9]
    f_end \last, 3
.endm
.macro f_alu_reg_stub id, last, op, r, flags, a4, a5
.endm

// [d_off][n_off][m_off][shift]: op, r
.macro f_alu_reg_lsl_body id, last, op, r, a3, a4, a5
    ldp x9, x10, [_ip]
    ldp x11, x14, [_ip, #16]
    ldr x12, [_cpu, x10]
    ldr x13, [_cpu, x11]
    lslv \r\()13, \r\()13, \r\()14
    \op \r\()12, \r\()12, \r\()13
    str x12, [_cpu, x9]
    f_end \last, 4
.endm
.macro f_alu_reg_lsl_stub id, last, op, r, a3, a4, a5
.endm

// Bitfield extract and the immediate shifts (UBFM, SBFM and their aliases):
// (src << L) >> S, logical or arithmetic -- dpextra.S has the lowering.
// [d_off][n_off][L][S]: shiftop, r
.macro f_bfm_body id, last, shiftop, r, a3, a4, a5
    ldp x9, x10, [_ip]
    ldp x11, x13, [_ip, #16]
    ldr x12, [_cpu, x10]
    lslv \r\()12, \r\()12, \r\()11
    \shiftop \r\()12, \r\()12, \r\()13
    str x12, [_cpu, x9]
    f_end \last, 4
.endm
.macro f_bfm_stub id, last, shiftop, r, a3, a4, a5
.endm

// ---- Moves ----------------------------------------------------------------

// Register to register, zero-extending (simd_spec.S's vspec_umov*, which is
// also what `mov Xd, Xm` and `mov Wd, Wm` become). [d_off][n_off]: ld, reg
.macro f_umov_body id, last, ld, reg, a3, a4, a5
    ldp x8, x10, [_ip]
    \ld \reg, [_cpu, x10]
    str x11, [_cpu, x8]
    f_end \last, 2
.endm
.macro f_umov_stub id, last, ld, reg, a3, a4, a5
.endm

// A constant (math.S's mov_const). [d_off][value]
.macro f_mov_const_body id, last, a1, a2, a3, a4, a5
    ldp x8, x9, [_ip]
    str x9, [_cpu, x8]
    f_end \last, 2
.endm
.macro f_mov_const_stub id, last, a1, a2, a3, a4, a5
.endm

// ---- Loads and stores, offset form (memory.S) ----------------------------

// [rt_slot][rn_slot][offset][orig_ip]: bits, ldinsn, reg
.macro f_load_body id, last, bits, ldinsn, reg, a4, a5
    ldp x8, x9, [_ip]         // rt_slot, rn_slot
    ldr x19, [_ip, #16]       // offset
    f_ip_past \last
    ldr x10, [_cpu, x9]       // base
    add x7, x10, x19
    f_read_prep \last, \bits, \id
    \ldinsn \reg, [_addr]
    str x17, [_cpu, x8]       // full X store: 32-bit loads must clear the upper half
    f_end_ahead \last
.endm
.macro f_load_stub id, last, bits, ldinsn, reg, a4, a5
    f_read_bullshit \last, \bits, \id
.endm

// [rt_slot][rn_slot][offset][orig_ip]: bits, stinsn, reg
.macro f_store_body id, last, bits, stinsn, reg, a4, a5
    ldp x8, x9, [_ip]         // rt_slot, rn_slot
    ldr x19, [_ip, #16]       // offset
    f_ip_past \last
    ldr x10, [_cpu, x9]       // base
    ldr x17, [_cpu, x8]       // value (x17 survives write_prep's helpers)
    add x7, x10, x19
    f_write_prep \last, \bits, \id
    \stinsn \reg, [_addr]
    write_done \bits, \id     // clobbers x9 — rt_slot already consumed
    f_end_ahead \last
.endm
.macro f_store_stub id, last, bits, stinsn, reg, a4, a5
    f_write_bullshit \last, \bits, \id
.endm

// Register-offset form: the index register extended (0: none, 1: sxtw,
// 2: uxtw) and shifted. [rt_slot][rn_slot][rm_slot][shift][orig_ip]:
// bits, insn, reg, ext
.macro rx_index ext
    ldr x11, [_cpu, x11]
.if \ext == 1
    sxtw x11, w11
.elseif \ext == 2
    mov w11, w11
.endif
    lslv x11, x11, x12
.endm

.macro f_load_rx_body id, last, bits, ldinsn, reg, ext, a5
    ldp x8, x9, [_ip]         // rt_slot, rn_slot
    ldp x11, x12, [_ip, #16]  // rm_slot, shift
    f_ip_past \last, 5
    ldr x10, [_cpu, x9]
    rx_index \ext
    add x7, x10, x11
    f_read_prep \last, \bits, \id
    \ldinsn \reg, [_addr]
    str x17, [_cpu, x8]
    f_end_ahead \last
.endm
.macro f_load_rx_stub id, last, bits, ldinsn, reg, ext, a5
    f_read_bullshit \last, \bits, \id
.endm

.macro f_store_rx_body id, last, bits, stinsn, reg, ext, a5
    ldp x8, x9, [_ip]         // rt_slot, rn_slot
    ldp x11, x12, [_ip, #16]  // rm_slot, shift
    f_ip_past \last, 5
    ldr x10, [_cpu, x9]
    rx_index \ext
    add x7, x10, x11
    ldr x17, [_cpu, x8]
    f_write_prep \last, \bits, \id
    \stinsn \reg, [_addr]
    write_done \bits, \id
    f_end_ahead \last
.endm
.macro f_store_rx_stub id, last, bits, stinsn, reg, ext, a5
    f_write_bullshit \last, \bits, \id
.endm

// Pairs, offset form. [rt_slot | rt2_slot<<16][rn_slot][offset][orig_ip]:
// bits, bytes, reg1, reg2
.macro f_ldp_body id, last, bits, bytes, ldr_reg1, ldr_reg2, a5
    ldp x8, x9, [_ip]          // [rt_slot | rt2_slot<<16], [rn_slot]
    ldr x19, [_ip, #16]        // offset
    f_ip_past \last
    ldr x10, [_cpu, x9]        // base from precomputed rn_slot
    add x7, x10, x19
    f_read_prep \last, \bits, \id
    ldr \ldr_reg1, [_addr]
    ldr \ldr_reg2, [_addr, #\bytes]
    and x9, x8, #0xffff        // rt_slot (low 16 bits)
    str x17, [_cpu, x9]        // full X store: 32-bit loads must clear upper half
    ubfx x9, x8, #16, 16      // rt2_slot (high 16 bits)
    str x25, [_cpu, x9]        // full X store: 32-bit loads must clear upper half
    f_end_ahead \last
.endm
.macro f_ldp_stub id, last, bits, bytes, ldr_reg1, ldr_reg2, a5
    f_read_bullshit \last, \bits, \id
.endm

.macro f_stp_body id, last, bits, bytes, str_reg1, str_reg2, a5
    ldp x8, x9, [_ip]          // [rt_slot | rt2_slot<<16], [rn_slot]
    ldr x19, [_ip, #16]        // offset
    f_ip_past \last
    and x21, x8, #0xffff       // rt_slot (callee-saved, survives write_prep)
    ubfx x22, x8, #16, #16    // rt2_slot (callee-saved, survives write_prep)
    ldr \str_reg1, [_cpu, x21] // load value from rt slot
    ldr \str_reg2, [_cpu, x22] // load value from rt2 slot
    ldr x10, [_cpu, x9]        // base from precomputed rn_slot
    add x7, x10, x19
    f_write_prep \last, \bits, \id
    str \str_reg1, [_addr]
    str \str_reg2, [_addr, #\bytes]
    write_done \bits, \id
    f_end_ahead \last
.endm
.macro f_stp_stub id, last, bits, bytes, str_reg1, str_reg2, a5
    f_write_bullshit \last, \bits, \id
.endm

// Pairs with writeback: the new base (base + offset) is computed before the
// access and written to rn after it, so a fault leaves rn untouched. x23
// holds the new base and x24 rn's slot across the prep helpers. Same stream
// as the offset form: bits, bytes, reg1, reg2, pre
.macro f_ldp_wb_body id, last, bits, bytes, ldr_reg1, ldr_reg2, pre
    ldp x8, x24, [_ip]
    ldr x19, [_ip, #16]
    f_ip_past \last
    ldr x10, [_cpu, x24]
    add x23, x10, x19
.if \pre
    mov x7, x23
.else
    mov x7, x10
.endif
    f_read_prep \last, \bits, \id
    ldr \ldr_reg1, [_addr]
    ldr \ldr_reg2, [_addr, #\bytes]
    and x9, x8, #0xffff
    str x17, [_cpu, x9]
    ubfx x9, x8, #16, 16
    str x25, [_cpu, x9]
    str x23, [_cpu, x24]
    f_end_ahead \last
.endm
.macro f_ldp_wb_stub id, last, bits, bytes, ldr_reg1, ldr_reg2, pre
    f_read_bullshit \last, \bits, \id
.endm

.macro f_stp_wb_body id, last, bits, bytes, str_reg1, str_reg2, pre
    ldp x8, x24, [_ip]
    ldr x19, [_ip, #16]
    f_ip_past \last
    and x21, x8, #0xffff
    ubfx x22, x8, #16, #16
    ldr \str_reg1, [_cpu, x21]
    ldr \str_reg2, [_cpu, x22]
    ldr x10, [_cpu, x24]
    add x23, x10, x19
.if \pre
    mov x7, x23
.else
    mov x7, x10
.endif
    f_write_prep \last, \bits, \id
    str \str_reg1, [_addr]
    str \str_reg2, [_addr, #\bytes]
    write_done \bits, \id
    str x23, [_cpu, x24]
    f_end_ahead \last
.endm
.macro f_stp_wb_stub id, last, bits, bytes, str_reg1, str_reg2, pre
    f_write_bullshit \last, \bits, \id
.endm

// A standalone gadget from a family, and a fused pair of two.
.macro f_gadget name, fam, a1, a2, a3, a4, a5
.gadget \name
    \fam\()_body \name, 1, \a1, \a2, \a3, \a4, \a5
    \fam\()_stub \name, 1, \a1, \a2, \a3, \a4, \a5
.endm

.macro fuse2 name, fa, a1, a2, a3, a4, a5, fb, b1, b2, b3, b4, b5
.gadget f2_\name
    \fa\()_body f2_\name\()_a, 0, \a1, \a2, \a3, \a4, \a5
    \fb\()_body f2_\name\()_b, 1, \b1, \b2, \b3, \b4, \b5
    \fa\()_stub f2_\name\()_a, 0, \a1, \a2, \a3, \a4, \a5
    \fb\()_stub f2_\name\()_b, 1, \b1, \b2, \b3, \b4, \b5
.endm
