// The fused pairs of the arm64 guest: which two gadgets, when one follows the
// other in a block, are replaced by one gadget that does both (frag.h has the
// how and the why). Read by the assembler, which builds a gadget for each
// FUSE2 line (alu_spec.S), and by the translator, which looks adjacent gadgets
// up in the same list (gen.c's arm64_pair_fuse). The `pair` bit of
// /proc/ish/arm64_jit_fuse turns the pass off.
//
// A gadget takes part once it has a line in the first table: F_<gadget> is
// its body's family and arguments (frag.h), padded to five with `_`, and
// NP_<gadget> the number of parameter words that follow it in the stream. A
// gadget that ends its block (a branch) has no NP_: it can only be the second
// of a pair, and that is the only place such gadgets appear in the profiles.
//
// The second table is chosen by measurement, not by hand: it is the commonest
// adjacent pairs of such gadgets in the gadget-level JIT profiles of Python,
// sqlite, gcc and 7-Zip (ISH_JIT_PROFILE, tools/jitprof-gadget-pairs.py
// --fuse), most frequent first. Regenerate it with
//
//     python3 tools/jitprof-gadget-pairs.py --fuse jit/guest-arm64/pairs.h PROFILE...
//
// after adding a family, or to weigh in another workload.

#define F_ospec_addi64 f_alu_imm, add, x, 0, _, _
#define NP_ospec_addi64 3
#define F_ospec_addsi64 f_alu_imm, adds, x, 1, _, _
#define NP_ospec_addsi64 3
#define F_ospec_addr64 f_alu_reg, add, x, 0, _, _
#define NP_ospec_addr64 3
#define F_ospec_addsr64 f_alu_reg, adds, x, 1, _, _
#define NP_ospec_addsr64 3
#define F_ospec_addr_lsl64 f_alu_reg_lsl, add, x, _, _, _
#define NP_ospec_addr_lsl64 4
#define F_ospec_addi32 f_alu_imm, add, w, 0, _, _
#define NP_ospec_addi32 3
#define F_ospec_addsi32 f_alu_imm, adds, w, 1, _, _
#define NP_ospec_addsi32 3
#define F_ospec_addr32 f_alu_reg, add, w, 0, _, _
#define NP_ospec_addr32 3
#define F_ospec_addsr32 f_alu_reg, adds, w, 1, _, _
#define NP_ospec_addsr32 3
#define F_ospec_addr_lsl32 f_alu_reg_lsl, add, w, _, _, _
#define NP_ospec_addr_lsl32 4
#define F_ospec_subi64 f_alu_imm, sub, x, 0, _, _
#define NP_ospec_subi64 3
#define F_ospec_subsi64 f_alu_imm, subs, x, 1, _, _
#define NP_ospec_subsi64 3
#define F_ospec_subr64 f_alu_reg, sub, x, 0, _, _
#define NP_ospec_subr64 3
#define F_ospec_subsr64 f_alu_reg, subs, x, 1, _, _
#define NP_ospec_subsr64 3
#define F_ospec_subr_lsl64 f_alu_reg_lsl, sub, x, _, _, _
#define NP_ospec_subr_lsl64 4
#define F_ospec_subi32 f_alu_imm, sub, w, 0, _, _
#define NP_ospec_subi32 3
#define F_ospec_subsi32 f_alu_imm, subs, w, 1, _, _
#define NP_ospec_subsi32 3
#define F_ospec_subr32 f_alu_reg, sub, w, 0, _, _
#define NP_ospec_subr32 3
#define F_ospec_subsr32 f_alu_reg, subs, w, 1, _, _
#define NP_ospec_subsr32 3
#define F_ospec_subr_lsl32 f_alu_reg_lsl, sub, w, _, _, _
#define NP_ospec_subr_lsl32 4
#define F_ospec_andi64 f_alu_imm, and, x, 0, _, _
#define NP_ospec_andi64 3
#define F_ospec_andr64 f_alu_reg, and, x, 0, _, _
#define NP_ospec_andr64 3
#define F_ospec_andi32 f_alu_imm, and, w, 0, _, _
#define NP_ospec_andi32 3
#define F_ospec_andr32 f_alu_reg, and, w, 0, _, _
#define NP_ospec_andr32 3
#define F_ospec_orri64 f_alu_imm, orr, x, 0, _, _
#define NP_ospec_orri64 3
#define F_ospec_orrr64 f_alu_reg, orr, x, 0, _, _
#define NP_ospec_orrr64 3
#define F_ospec_orri32 f_alu_imm, orr, w, 0, _, _
#define NP_ospec_orri32 3
#define F_ospec_orrr32 f_alu_reg, orr, w, 0, _, _
#define NP_ospec_orrr32 3
#define F_ospec_eori64 f_alu_imm, eor, x, 0, _, _
#define NP_ospec_eori64 3
#define F_ospec_eorr64 f_alu_reg, eor, x, 0, _, _
#define NP_ospec_eorr64 3
#define F_ospec_eori32 f_alu_imm, eor, w, 0, _, _
#define NP_ospec_eori32 3
#define F_ospec_eorr32 f_alu_reg, eor, w, 0, _, _
#define NP_ospec_eorr32 3
#define F_ospec_andsi64 f_alu_imm, ands, x, 1, _, _
#define NP_ospec_andsi64 3
#define F_ospec_andsr64 f_alu_reg, ands, x, 1, _, _
#define NP_ospec_andsr64 3
#define F_ospec_ubfm64 f_bfm, lsrv, x, _, _, _
#define NP_ospec_ubfm64 4
#define F_ospec_sbfm64 f_bfm, asrv, x, _, _, _
#define NP_ospec_sbfm64 4
#define F_ospec_andsi32 f_alu_imm, ands, w, 1, _, _
#define NP_ospec_andsi32 3
#define F_ospec_andsr32 f_alu_reg, ands, w, 1, _, _
#define NP_ospec_andsr32 3
#define F_ospec_ubfm32 f_bfm, lsrv, w, _, _, _
#define NP_ospec_ubfm32 4
#define F_ospec_sbfm32 f_bfm, asrv, w, _, _, _
#define NP_ospec_sbfm32 4
#define F_vspec_umov64 f_umov, ldr, x11, _, _, _
#define NP_vspec_umov64 2
#define F_vspec_umov32 f_umov, ldr, w11, _, _, _
#define NP_vspec_umov32 2
#define F_mov_const f_mov_const, _, _, _, _, _
#define NP_mov_const 2
#define F_load8_fast f_load, 8, ldrb, w17, _, _
#define NP_load8_fast 4
#define F_load16_fast f_load, 16, ldrh, w17, _, _
#define NP_load16_fast 4
#define F_load32_fast f_load, 32, ldr, w17, _, _
#define NP_load32_fast 4
#define F_load64_fast f_load, 64, ldr, x17, _, _
#define NP_load64_fast 4
#define F_loads8_32_fast f_load, 8, ldrsb, w17, _, _
#define NP_loads8_32_fast 4
#define F_loads8_64_fast f_load, 8, ldrsb, x17, _, _
#define NP_loads8_64_fast 4
#define F_loads16_32_fast f_load, 16, ldrsh, w17, _, _
#define NP_loads16_32_fast 4
#define F_loads16_64_fast f_load, 16, ldrsh, x17, _, _
#define NP_loads16_64_fast 4
#define F_loads32_64_fast f_load, 32, ldrsw, x17, _, _
#define NP_loads32_64_fast 4
#define F_store8_fast f_store, 8, strb, w17, _, _
#define NP_store8_fast 4
#define F_store16_fast f_store, 16, strh, w17, _, _
#define NP_store16_fast 4
#define F_store32_fast f_store, 32, str, w17, _, _
#define NP_store32_fast 4
#define F_store64_fast f_store, 64, str, x17, _, _
#define NP_store64_fast 4
#define F_load8_rxl f_load_rx, 8, ldrb, w17, 0, _
#define NP_load8_rxl 5
#define F_load8_rxs f_load_rx, 8, ldrb, w17, 1, _
#define NP_load8_rxs 5
#define F_load8_rxu f_load_rx, 8, ldrb, w17, 2, _
#define NP_load8_rxu 5
#define F_load16_rxl f_load_rx, 16, ldrh, w17, 0, _
#define NP_load16_rxl 5
#define F_load16_rxs f_load_rx, 16, ldrh, w17, 1, _
#define NP_load16_rxs 5
#define F_load16_rxu f_load_rx, 16, ldrh, w17, 2, _
#define NP_load16_rxu 5
#define F_load32_rxl f_load_rx, 32, ldr, w17, 0, _
#define NP_load32_rxl 5
#define F_load32_rxs f_load_rx, 32, ldr, w17, 1, _
#define NP_load32_rxs 5
#define F_load32_rxu f_load_rx, 32, ldr, w17, 2, _
#define NP_load32_rxu 5
#define F_load64_rxl f_load_rx, 64, ldr, x17, 0, _
#define NP_load64_rxl 5
#define F_load64_rxs f_load_rx, 64, ldr, x17, 1, _
#define NP_load64_rxs 5
#define F_load64_rxu f_load_rx, 64, ldr, x17, 2, _
#define NP_load64_rxu 5
#define F_loads8_32_rxl f_load_rx, 8, ldrsb, w17, 0, _
#define NP_loads8_32_rxl 5
#define F_loads8_32_rxs f_load_rx, 8, ldrsb, w17, 1, _
#define NP_loads8_32_rxs 5
#define F_loads8_32_rxu f_load_rx, 8, ldrsb, w17, 2, _
#define NP_loads8_32_rxu 5
#define F_loads8_64_rxl f_load_rx, 8, ldrsb, x17, 0, _
#define NP_loads8_64_rxl 5
#define F_loads8_64_rxs f_load_rx, 8, ldrsb, x17, 1, _
#define NP_loads8_64_rxs 5
#define F_loads8_64_rxu f_load_rx, 8, ldrsb, x17, 2, _
#define NP_loads8_64_rxu 5
#define F_loads16_32_rxl f_load_rx, 16, ldrsh, w17, 0, _
#define NP_loads16_32_rxl 5
#define F_loads16_32_rxs f_load_rx, 16, ldrsh, w17, 1, _
#define NP_loads16_32_rxs 5
#define F_loads16_32_rxu f_load_rx, 16, ldrsh, w17, 2, _
#define NP_loads16_32_rxu 5
#define F_loads16_64_rxl f_load_rx, 16, ldrsh, x17, 0, _
#define NP_loads16_64_rxl 5
#define F_loads16_64_rxs f_load_rx, 16, ldrsh, x17, 1, _
#define NP_loads16_64_rxs 5
#define F_loads16_64_rxu f_load_rx, 16, ldrsh, x17, 2, _
#define NP_loads16_64_rxu 5
#define F_loads32_64_rxl f_load_rx, 32, ldrsw, x17, 0, _
#define NP_loads32_64_rxl 5
#define F_loads32_64_rxs f_load_rx, 32, ldrsw, x17, 1, _
#define NP_loads32_64_rxs 5
#define F_loads32_64_rxu f_load_rx, 32, ldrsw, x17, 2, _
#define NP_loads32_64_rxu 5
#define F_store8_rxl f_store_rx, 8, strb, w17, 0, _
#define NP_store8_rxl 5
#define F_store8_rxs f_store_rx, 8, strb, w17, 1, _
#define NP_store8_rxs 5
#define F_store8_rxu f_store_rx, 8, strb, w17, 2, _
#define NP_store8_rxu 5
#define F_store16_rxl f_store_rx, 16, strh, w17, 0, _
#define NP_store16_rxl 5
#define F_store16_rxs f_store_rx, 16, strh, w17, 1, _
#define NP_store16_rxs 5
#define F_store16_rxu f_store_rx, 16, strh, w17, 2, _
#define NP_store16_rxu 5
#define F_store32_rxl f_store_rx, 32, str, w17, 0, _
#define NP_store32_rxl 5
#define F_store32_rxs f_store_rx, 32, str, w17, 1, _
#define NP_store32_rxs 5
#define F_store32_rxu f_store_rx, 32, str, w17, 2, _
#define NP_store32_rxu 5
#define F_store64_rxl f_store_rx, 64, str, x17, 0, _
#define NP_store64_rxl 5
#define F_store64_rxs f_store_rx, 64, str, x17, 1, _
#define NP_store64_rxs 5
#define F_store64_rxu f_store_rx, 64, str, x17, 2, _
#define NP_store64_rxu 5
#define F_ldp64_fast f_ldp, 128, 8, x17, x25, _
#define NP_ldp64_fast 4
#define F_stp64_fast f_stp, 128, 8, x17, x25, _
#define NP_stp64_fast 4
#define F_ldp64_post f_ldp_wb, 128, 8, x17, x25, 0
#define NP_ldp64_post 4
#define F_stp64_pre f_stp_wb, 128, 8, x17, x25, 1
#define NP_stp64_pre 4
#define F_b f_b, _, _, _, _, _
#define F_bl f_bl, _, _, _, _, _
#define F_br_cached f_br_cached, _, _, _, _, _
#define F_blr_cached f_blr_cached, _, _, _, _, _
#define F_ret_cached f_br_cached, _, _, _, _, _
#define F_cbz_x f_cb, x9, cbz, _, _, _
#define F_cbz_x_bk f_cb_bk, x9, cbnz, _, _, _
#define F_cbz_w f_cb, w9, cbz, _, _, _
#define F_cbz_w_bk f_cb_bk, w9, cbnz, _, _, _
#define F_cbnz_x f_cb, x9, cbnz, _, _, _
#define F_cbnz_x_bk f_cb_bk, x9, cbz, _, _, _
#define F_cbnz_w f_cb, w9, cbnz, _, _, _
#define F_cbnz_w_bk f_cb_bk, w9, cbz, _, _, _
#define F_tb_z f_tb, cbz, _, _, _, _
#define F_tb_z_bk f_tb_bk, cbnz, _, _, _, _
#define F_tb_nz f_tb, cbnz, _, _, _, _
#define F_tb_nz_bk f_tb_bk, cbz, _, _, _, _
#define F_bcond_eq f_bcond, eq, _, _, _, _
#define F_bcond_nf_eq f_bcond_nf, eq, _, _, _, _
#define F_fused_cmpi64_eq f_fused_cmpi, subs, xzr, x15, x11, eq
#define F_fused_cmpi32_eq f_fused_cmpi, subs, wzr, w15, w11, eq
#define F_fused_cmpr64_eq f_fused_cmpr, subs, xzr, x15, x13, eq
#define F_fused_cmpr32_eq f_fused_cmpr, subs, wzr, w15, w13, eq
#define F_bcond_ne f_bcond, ne, _, _, _, _
#define F_bcond_nf_ne f_bcond_nf, ne, _, _, _, _
#define F_fused_cmpi64_ne f_fused_cmpi, subs, xzr, x15, x11, ne
#define F_fused_cmpi32_ne f_fused_cmpi, subs, wzr, w15, w11, ne
#define F_fused_cmpr64_ne f_fused_cmpr, subs, xzr, x15, x13, ne
#define F_fused_cmpr32_ne f_fused_cmpr, subs, wzr, w15, w13, ne
#define F_bcond_cs f_bcond, cs, _, _, _, _
#define F_bcond_nf_cs f_bcond_nf, cs, _, _, _, _
#define F_fused_cmpi64_cs f_fused_cmpi, subs, xzr, x15, x11, cs
#define F_fused_cmpi32_cs f_fused_cmpi, subs, wzr, w15, w11, cs
#define F_fused_cmpr64_cs f_fused_cmpr, subs, xzr, x15, x13, cs
#define F_fused_cmpr32_cs f_fused_cmpr, subs, wzr, w15, w13, cs
#define F_bcond_cc f_bcond, cc, _, _, _, _
#define F_bcond_nf_cc f_bcond_nf, cc, _, _, _, _
#define F_fused_cmpi64_cc f_fused_cmpi, subs, xzr, x15, x11, cc
#define F_fused_cmpi32_cc f_fused_cmpi, subs, wzr, w15, w11, cc
#define F_fused_cmpr64_cc f_fused_cmpr, subs, xzr, x15, x13, cc
#define F_fused_cmpr32_cc f_fused_cmpr, subs, wzr, w15, w13, cc
#define F_bcond_mi f_bcond, mi, _, _, _, _
#define F_bcond_nf_mi f_bcond_nf, mi, _, _, _, _
#define F_fused_cmpi64_mi f_fused_cmpi, subs, xzr, x15, x11, mi
#define F_fused_cmpi32_mi f_fused_cmpi, subs, wzr, w15, w11, mi
#define F_fused_cmpr64_mi f_fused_cmpr, subs, xzr, x15, x13, mi
#define F_fused_cmpr32_mi f_fused_cmpr, subs, wzr, w15, w13, mi
#define F_bcond_pl f_bcond, pl, _, _, _, _
#define F_bcond_nf_pl f_bcond_nf, pl, _, _, _, _
#define F_fused_cmpi64_pl f_fused_cmpi, subs, xzr, x15, x11, pl
#define F_fused_cmpi32_pl f_fused_cmpi, subs, wzr, w15, w11, pl
#define F_fused_cmpr64_pl f_fused_cmpr, subs, xzr, x15, x13, pl
#define F_fused_cmpr32_pl f_fused_cmpr, subs, wzr, w15, w13, pl
#define F_bcond_vs f_bcond, vs, _, _, _, _
#define F_bcond_nf_vs f_bcond_nf, vs, _, _, _, _
#define F_fused_cmpi64_vs f_fused_cmpi, subs, xzr, x15, x11, vs
#define F_fused_cmpi32_vs f_fused_cmpi, subs, wzr, w15, w11, vs
#define F_fused_cmpr64_vs f_fused_cmpr, subs, xzr, x15, x13, vs
#define F_fused_cmpr32_vs f_fused_cmpr, subs, wzr, w15, w13, vs
#define F_bcond_vc f_bcond, vc, _, _, _, _
#define F_bcond_nf_vc f_bcond_nf, vc, _, _, _, _
#define F_fused_cmpi64_vc f_fused_cmpi, subs, xzr, x15, x11, vc
#define F_fused_cmpi32_vc f_fused_cmpi, subs, wzr, w15, w11, vc
#define F_fused_cmpr64_vc f_fused_cmpr, subs, xzr, x15, x13, vc
#define F_fused_cmpr32_vc f_fused_cmpr, subs, wzr, w15, w13, vc
#define F_bcond_hi f_bcond, hi, _, _, _, _
#define F_bcond_nf_hi f_bcond_nf, hi, _, _, _, _
#define F_fused_cmpi64_hi f_fused_cmpi, subs, xzr, x15, x11, hi
#define F_fused_cmpi32_hi f_fused_cmpi, subs, wzr, w15, w11, hi
#define F_fused_cmpr64_hi f_fused_cmpr, subs, xzr, x15, x13, hi
#define F_fused_cmpr32_hi f_fused_cmpr, subs, wzr, w15, w13, hi
#define F_bcond_ls f_bcond, ls, _, _, _, _
#define F_bcond_nf_ls f_bcond_nf, ls, _, _, _, _
#define F_fused_cmpi64_ls f_fused_cmpi, subs, xzr, x15, x11, ls
#define F_fused_cmpi32_ls f_fused_cmpi, subs, wzr, w15, w11, ls
#define F_fused_cmpr64_ls f_fused_cmpr, subs, xzr, x15, x13, ls
#define F_fused_cmpr32_ls f_fused_cmpr, subs, wzr, w15, w13, ls
#define F_bcond_ge f_bcond, ge, _, _, _, _
#define F_bcond_nf_ge f_bcond_nf, ge, _, _, _, _
#define F_fused_cmpi64_ge f_fused_cmpi, subs, xzr, x15, x11, ge
#define F_fused_cmpi32_ge f_fused_cmpi, subs, wzr, w15, w11, ge
#define F_fused_cmpr64_ge f_fused_cmpr, subs, xzr, x15, x13, ge
#define F_fused_cmpr32_ge f_fused_cmpr, subs, wzr, w15, w13, ge
#define F_bcond_lt f_bcond, lt, _, _, _, _
#define F_bcond_nf_lt f_bcond_nf, lt, _, _, _, _
#define F_fused_cmpi64_lt f_fused_cmpi, subs, xzr, x15, x11, lt
#define F_fused_cmpi32_lt f_fused_cmpi, subs, wzr, w15, w11, lt
#define F_fused_cmpr64_lt f_fused_cmpr, subs, xzr, x15, x13, lt
#define F_fused_cmpr32_lt f_fused_cmpr, subs, wzr, w15, w13, lt
#define F_bcond_gt f_bcond, gt, _, _, _, _
#define F_bcond_nf_gt f_bcond_nf, gt, _, _, _, _
#define F_fused_cmpi64_gt f_fused_cmpi, subs, xzr, x15, x11, gt
#define F_fused_cmpi32_gt f_fused_cmpi, subs, wzr, w15, w11, gt
#define F_fused_cmpr64_gt f_fused_cmpr, subs, xzr, x15, x13, gt
#define F_fused_cmpr32_gt f_fused_cmpr, subs, wzr, w15, w13, gt
#define F_bcond_le f_bcond, le, _, _, _, _
#define F_bcond_nf_le f_bcond_nf, le, _, _, _, _
#define F_fused_cmpi64_le f_fused_cmpi, subs, xzr, x15, x11, le
#define F_fused_cmpi32_le f_fused_cmpi, subs, wzr, w15, w11, le
#define F_fused_cmpr64_le f_fused_cmpr, subs, xzr, x15, x13, le
#define F_fused_cmpr32_le f_fused_cmpr, subs, wzr, w15, w13, le

// ---- generated: the pairs ----
FUSE2(ospec_eorr32, ospec_addr32)
FUSE2(ospec_addr32, ospec_eorr32)
FUSE2(mov_const, load64_fast)
FUSE2(load64_fast, load64_fast)
FUSE2(load64_fast, ospec_addi64)
FUSE2(ldp64_post, ret_cached)
FUSE2(ospec_addi64, stp64_fast)
FUSE2(stp64_fast, vspec_umov64)
FUSE2(ldp64_fast, ldp64_fast)
FUSE2(vspec_umov64, vspec_umov64)
FUSE2(load64_fast, store64_fast)
FUSE2(stp64_pre, ospec_addi64)
FUSE2(ospec_addi64, br_cached)
FUSE2(store64_fast, store64_fast)
FUSE2(ldp64_fast, ldp64_post)
FUSE2(vspec_umov64, bl)
FUSE2(load64_fast, vspec_umov64)
FUSE2(mov_const, mov_const)
FUSE2(ospec_addi64, store64_fast)
FUSE2(ospec_ubfm32, ospec_ubfm32)
FUSE2(load64_fast, cbz_x)
FUSE2(load8_fast, cbnz_w_bk)
FUSE2(ospec_addi64, fused_cmpi32_hi)
FUSE2(load64_fast, mov_const)
FUSE2(ospec_ubfm32, ospec_andi32)
FUSE2(load64_fast, load8_fast)
FUSE2(vspec_umov64, load64_fast)
FUSE2(ospec_addi64, load64_fast)
FUSE2(ospec_ubfm64, ospec_addr32)
FUSE2(vspec_umov64, stp64_fast)
FUSE2(load16_rxl, fused_cmpr32_hi)
FUSE2(load64_fast, load32_fast)
FUSE2(store64_fast, mov_const)
FUSE2(load64_fast, cbnz_x)
FUSE2(store64_fast, vspec_umov64)
FUSE2(mov_const, ospec_addi64)
FUSE2(load16_rxu, fused_cmpr32_hi)
FUSE2(load32_fast, load64_fast)
FUSE2(load64_fast, ret_cached)
FUSE2(vspec_umov64, ospec_addi64)
FUSE2(ospec_addi32, ospec_subr32)
FUSE2(vspec_umov64, mov_const)
FUSE2(ospec_addi64, mov_const)
FUSE2(load64_rxs, br_cached)
FUSE2(ospec_andi32, load64_rxs)
FUSE2(ospec_subr32, ospec_ubfm64)
FUSE2(load32_fast, load32_fast)
FUSE2(mov_const, store64_fast)
FUSE2(ospec_addi64, ospec_addi64)
FUSE2(ospec_subi64, store64_fast)
FUSE2(stp64_fast, ospec_addi64)
FUSE2(ospec_subr32, b)
FUSE2(load64_fast, ospec_addr64)
FUSE2(ospec_addi32, store32_fast)
FUSE2(load32_fast, mov_const)
FUSE2(ospec_addr32, store16_fast)
FUSE2(store64_fast, load64_fast)
FUSE2(vspec_umov64, vspec_umov32)
FUSE2(load8_fast, ospec_addi64)
FUSE2(store64_fast, ospec_addi64)
FUSE2(ospec_addi64, blr_cached)
FUSE2(vspec_umov32, vspec_umov64)
FUSE2(mov_const, load32_fast)
FUSE2(ospec_addi64, b)
FUSE2(vspec_umov64, ldp64_fast)
FUSE2(load8_fast, fused_cmpi32_eq)
FUSE2(load16_rxu, mov_const)
FUSE2(ospec_subi64, load64_fast)
FUSE2(stp64_fast, stp64_fast)
FUSE2(store16_rxl, ospec_ubfm64)
FUSE2(load64_fast, ldp64_fast)
FUSE2(ospec_addr32, store16_rxl)
FUSE2(load8_fast, fused_cmpr32_ne)
FUSE2(mov_const, b)
FUSE2(load64_rxl, load64_fast)
FUSE2(load64_fast, stp64_fast)
FUSE2(store64_fast, fused_cmpi32_hi)
FUSE2(stp64_fast, mov_const)
FUSE2(ospec_andi32, ospec_ubfm32)
FUSE2(stp64_fast, vspec_umov32)
FUSE2(store64_fast, bl)
FUSE2(load64_fast, load16_rxu)
FUSE2(stp64_pre, mov_const)
FUSE2(load8_fast, cbnz_w)
FUSE2(ospec_addi64, bl)
FUSE2(load64_fast, tb_nz)
FUSE2(stp64_pre, vspec_umov64)
FUSE2(ospec_ubfm64, ospec_addr_lsl64)
FUSE2(load64_fast, ospec_ubfm32)
FUSE2(load64_fast, ospec_subi64)
FUSE2(ospec_addi32, ospec_andr32)
FUSE2(ospec_ubfm64, load16_rxu)
FUSE2(load64_fast, load16_fast)
FUSE2(mov_const, ldp64_fast)
FUSE2(stp64_fast, load64_fast)
FUSE2(mov_const, bcond_ne)
FUSE2(load64_fast, ospec_subsr64)
FUSE2(vspec_umov64, cbz_x)
FUSE2(ospec_subsr64, mov_const)
FUSE2(load64_fast, ospec_ubfm64)
FUSE2(ospec_addr32, bcond_ne)
FUSE2(ospec_addr_lsl32, ospec_addr32)
FUSE2(load8_rxu, load8_rxu)
FUSE2(store32_fast, load64_fast)
FUSE2(load8_fast, load8_fast)
FUSE2(mov_const, bl)
FUSE2(ospec_sbfm32, ospec_addi32)
FUSE2(ospec_subr32, ospec_sbfm32)
FUSE2(ospec_ubfm32, ospec_subsi32)
FUSE2(ospec_subsi32, ospec_subr32)
FUSE2(ospec_andr32, ospec_addr_lsl32)
FUSE2(ospec_addr32, ospec_addr32)
FUSE2(vspec_umov64, load8_fast)
FUSE2(load64_fast, ldp64_post)
FUSE2(load64_fast, tb_z)
FUSE2(ospec_andi64, load64_fast)
FUSE2(ospec_addi32, fused_cmpr32_eq)
FUSE2(load32_rxl, ospec_addr32)
FUSE2(store64_fast, ret_cached)
FUSE2(load64_fast, ospec_andi64)
FUSE2(ospec_subi32, ospec_andi32)
FUSE2(load8_fast, cbz_w)
FUSE2(ospec_addr64, load64_fast)
FUSE2(ospec_addr_lsl64, load64_fast)
FUSE2(ospec_subi64, ospec_subi64)
FUSE2(vspec_umov32, stp64_fast)
FUSE2(ospec_andi32, tb_z)
FUSE2(ospec_addi64, ospec_subi64)
FUSE2(vspec_umov32, vspec_umov32)
FUSE2(load64_fast, bl)
FUSE2(mov_const, stp64_fast)
FUSE2(ldp64_fast, vspec_umov64)
FUSE2(vspec_umov64, store64_fast)
FUSE2(ospec_addr_lsl64, load16_rxl)
FUSE2(ospec_ubfm32, load16_fast)
FUSE2(load16_fast, load16_rxs)
FUSE2(load8_rxu, fused_cmpr32_eq)
FUSE2(load32_fast, tb_nz)
FUSE2(ospec_addi64, vspec_umov64)
FUSE2(load16_fast, ospec_addi64)
FUSE2(vspec_umov32, b)
FUSE2(ospec_andi32, mov_const)
FUSE2(vspec_umov64, b)
FUSE2(load8_fast, ospec_andi32)
FUSE2(load64_fast, blr_cached)
FUSE2(ospec_addi64, load16_fast)
FUSE2(vspec_umov32, ospec_addi64)
FUSE2(load32_fast, ospec_addi64)
FUSE2(ospec_subr32, ospec_subr32)
FUSE2(mov_const, ospec_orrr32)
FUSE2(vspec_umov32, ldp64_fast)
FUSE2(ldp64_fast, ospec_ubfm32)
FUSE2(load16_rxs, load8_fast)
FUSE2(loads8_64_fast, tb_z)
FUSE2(loads8_32_fast, ospec_andi32)
FUSE2(ospec_orrr32, b)
FUSE2(ospec_ubfm64, ospec_addr64)
FUSE2(vspec_umov64, blr_cached)
FUSE2(store64_fast, store32_fast)
FUSE2(load64_fast, ospec_andi32)
FUSE2(ospec_addi64, ospec_ubfm32)
FUSE2(load16_fast, load64_fast)
FUSE2(load32_fast, cbnz_w)
FUSE2(load32_fast, vspec_umov64)
FUSE2(mov_const, ospec_subr32)
FUSE2(load16_fast, mov_const)
FUSE2(vspec_umov64, load32_fast)
FUSE2(load32_fast, cbz_w)
FUSE2(ospec_subi32, fused_cmpi32_ls)
FUSE2(mov_const, store32_fast)
FUSE2(store16_fast, ospec_addr_lsl64)
FUSE2(ospec_eorr32, ospec_ubfm64)
FUSE2(load64_fast, b)
FUSE2(load16_fast, ospec_ubfm32)
FUSE2(ldp64_fast, vspec_umov32)
FUSE2(mov_const, vspec_umov64)
FUSE2(ospec_addi64, load32_rxl)
FUSE2(vspec_umov64, ldp64_post)
FUSE2(load64_fast, cbnz_x_bk)
FUSE2(load32_fast, store32_fast)
FUSE2(stp64_fast, store64_fast)
FUSE2(store32_fast, store64_fast)
FUSE2(ospec_sbfm32, b)
FUSE2(ospec_addr32, ospec_sbfm32)
FUSE2(store64_fast, cbz_x)
FUSE2(store32_fast, ospec_addi64)
FUSE2(store64_fast, ldp64_fast)
FUSE2(load64_fast, vspec_umov32)
FUSE2(mov_const, ospec_addr64)
FUSE2(load8_fast, load64_fast)
FUSE2(ospec_addi64, fused_cmpi64_ne)
FUSE2(load8_rxl, load8_rxl)
FUSE2(ospec_ubfm64, load32_rxl)
FUSE2(load16_fast, fused_cmpr32_hi)
FUSE2(load32_fast, ospec_addr32)
FUSE2(vspec_umov32, bl)
FUSE2(ospec_ubfm64, load64_rxl)
FUSE2(load32_fast, fused_cmpi32_hi)
FUSE2(load8_fast, fused_cmpi32_ne)
FUSE2(mov_const, vspec_umov32)
FUSE2(ospec_ubfm32, ospec_ubfm64)
FUSE2(ospec_andi32, fused_cmpi32_hi)
FUSE2(load32_fast, store64_fast)
FUSE2(ospec_addi64, ospec_addr64)
FUSE2(load16_fast, vspec_umov64)
FUSE2(vspec_umov32, mov_const)
FUSE2(ospec_addr64, ospec_addi64)
FUSE2(store32_fast, vspec_umov32)
FUSE2(ospec_subi64, ospec_ubfm64)
FUSE2(load16_fast, ospec_subi32)
FUSE2(ospec_addi64, load32_fast)
FUSE2(store32_fast, mov_const)
FUSE2(load64_fast, loads32_64_fast)
FUSE2(vspec_umov64, ospec_subi64)
FUSE2(store32_fast, b)
FUSE2(ospec_ubfm64, load16_rxl)
FUSE2(load8_fast, ospec_subi32)
FUSE2(ospec_ubfm64, ospec_subr32)
FUSE2(store64_fast, load8_fast)
FUSE2(store32_fast, vspec_umov64)
FUSE2(ospec_ubfm32, load64_rxl)
FUSE2(ospec_addr_lsl64, ospec_addi64)
FUSE2(load16_fast, fused_cmpi32_eq)
FUSE2(vspec_umov64, load16_fast)
FUSE2(ldp64_fast, mov_const)
FUSE2(store32_fast, store32_fast)
FUSE2(mov_const, load8_fast)
FUSE2(ospec_ubfm64, mov_const)
FUSE2(mov_const, ret_cached)
FUSE2(mov_const, ospec_ubfm64)
FUSE2(ospec_andi32, load64_fast)
FUSE2(ospec_ubfm32, ospec_addr_lsl32)
FUSE2(load64_fast, cbz_x_bk)
FUSE2(load8_fast, vspec_umov64)
FUSE2(stp64_fast, ospec_ubfm32)
FUSE2(ospec_addr64, ospec_addr64)
FUSE2(ospec_ubfm64, store16_fast)
FUSE2(store16_fast, load16_rxl)
FUSE2(ospec_addr64, load16_rxl)
FUSE2(store64_fast, ospec_orri64)
FUSE2(ospec_addr64, store64_fast)
FUSE2(ospec_subr32, mov_const)
FUSE2(store64_fast, ospec_ubfm32)
FUSE2(ospec_addr_lsl32, ospec_subi64)
FUSE2(ospec_andi32, ospec_ubfm64)
FUSE2(mov_const, store8_fast)
FUSE2(ospec_ubfm32, tb_z)
FUSE2(ldp64_post, b)
FUSE2(load16_fast, cbz_w)
FUSE2(load64_rxl, cbz_x)
FUSE2(store8_rxl, ospec_addi64)
FUSE2(vspec_umov32, store64_fast)
FUSE2(ospec_addr_lsl64, load32_fast)
FUSE2(ospec_andi64, ospec_orrr64)
FUSE2(ospec_subr32, fused_cmpr32_hi)
FUSE2(store16_fast, mov_const)
FUSE2(ospec_addr32, load32_fast)
FUSE2(load8_fast, mov_const)
FUSE2(ospec_addi32, fused_cmpr32_lt)
FUSE2(ospec_addr32, mov_const)
FUSE2(ospec_subi32, store32_fast)
FUSE2(ospec_addr_lsl32, ospec_addr_lsl64)
FUSE2(load64_rxs, cbz_x)
FUSE2(ospec_addi64, fused_cmpr64_ne)
FUSE2(ospec_ubfm64, ospec_andi32)
FUSE2(ospec_addr32, ospec_addi32)
FUSE2(ospec_ubfm32, load64_fast)
FUSE2(ospec_subr64, ospec_sbfm64)
FUSE2(store64_fast, ospec_subi32)
FUSE2(ospec_addi64, store32_fast)
FUSE2(load8_rxl, fused_cmpr32_eq)
FUSE2(load8_fast, load32_rxl)
FUSE2(load16_fast, store64_fast)
FUSE2(ospec_addi32, ospec_ubfm64)
FUSE2(mov_const, store16_fast)
FUSE2(store64_fast, cbnz_x)
FUSE2(vspec_umov64, ospec_ubfm64)
FUSE2(load64_fast, bcond_ne)
FUSE2(ospec_addi32, load32_fast)
FUSE2(load16_fast, load16_fast)
FUSE2(store64_fast, ospec_subi64)
FUSE2(load16_fast, fused_cmpi32_ne)
FUSE2(load8_fast, tb_nz)
FUSE2(load32_fast, ospec_addi32)
FUSE2(store32_fast, fused_cmpi32_hi)
FUSE2(load64_fast, ospec_addi32)
FUSE2(ospec_ubfm64, load64_rxs)
FUSE2(ospec_addi32, vspec_umov32)
FUSE2(stp64_fast, load32_fast)
FUSE2(ldp64_fast, load64_fast)
FUSE2(ospec_addi64, fused_cmpi32_ne)
FUSE2(vspec_umov32, load64_fast)
FUSE2(store64_fast, load32_fast)
FUSE2(load64_rxs, ospec_subi64)
FUSE2(ospec_addr32, ospec_addi64)
FUSE2(vspec_umov32, ospec_ubfm64)
FUSE2(vspec_umov32, cbz_w)
FUSE2(load32_rxl, load32_rxl)
FUSE2(ospec_addi64, load8_fast)
FUSE2(ospec_addi64, load64_rxl)
FUSE2(load64_rxl, store64_fast)
FUSE2(ospec_andi32, store8_fast)
FUSE2(ospec_subr64, load8_fast)
FUSE2(load32_rxl, ospec_subr64)
FUSE2(load8_fast, store32_fast)
FUSE2(load32_fast, ospec_addr_lsl64)
FUSE2(store64_fast, ospec_andi64)
FUSE2(store64_fast, tb_z)
FUSE2(load32_fast, ospec_andi64)
FUSE2(stp64_fast, ospec_andi32)
FUSE2(mov_const, load16_fast)
FUSE2(store16_fast, b)
FUSE2(ospec_addi64, load8_rxl)
FUSE2(load8_fast, ospec_addr_lsl64)
FUSE2(ospec_ubfm64, ospec_addr_lsl32)
FUSE2(ospec_addr_lsl32, ospec_subr_lsl32)
FUSE2(store16_fast, load64_fast)
FUSE2(ospec_andi32, tb_nz)
FUSE2(ospec_addr64, load8_fast)
FUSE2(ospec_subr32, ospec_eorr32)
FUSE2(ospec_subi64, mov_const)
FUSE2(ospec_addr_lsl64, ospec_addi32)
FUSE2(ospec_addr32, store32_fast)
FUSE2(ospec_addi64, ospec_addi32)
FUSE2(ospec_subi32, fused_cmpr32_le)
FUSE2(ospec_andi64, store64_fast)
FUSE2(load8_rxl, load8_fast)
FUSE2(ospec_addr_lsl64, ospec_addr32)
FUSE2(load64_fast, br_cached)
FUSE2(ospec_orrr64, store64_fast)
FUSE2(load64_fast, ospec_subr64)
FUSE2(ospec_sbfm64, ospec_addi64)
FUSE2(load16_rxl, ospec_eorr32)
FUSE2(ospec_subr32, ospec_addi32)
FUSE2(ospec_addr_lsl32, ospec_andr32)
FUSE2(load32_fast, fused_cmpi32_ne)
FUSE2(store64_fast, ospec_addi32)
FUSE2(mov_const, fused_cmpr32_hi)
FUSE2(store32_fast, store16_fast)
FUSE2(ospec_subi32, ospec_ubfm32)
FUSE2(store32_fast, cbnz_w)
FUSE2(load32_fast, tb_z)
FUSE2(store64_fast, b)
FUSE2(ospec_addr32, ospec_subi32)
FUSE2(vspec_umov32, ospec_addi32)
FUSE2(load8_fast, load16_fast)
FUSE2(ospec_addi32, fused_cmpr32_ne)
FUSE2(load64_fast, load64_rxl)
FUSE2(ospec_addi64, cbz_w)
FUSE2(load64_fast, ospec_sbfm64)
FUSE2(ospec_subi64, fused_cmpi64_hi)
FUSE2(vspec_umov64, loads8_32_fast)
FUSE2(store32_fast, ldp64_fast)
FUSE2(ospec_subi32, ospec_andr32)
FUSE2(load64_fast, ospec_addr_lsl64)
FUSE2(store32_fast, load32_fast)
FUSE2(ospec_addr64, load8_rxl)
FUSE2(ospec_addr64, load64_rxl)
FUSE2(load64_fast, load16_rxl)
FUSE2(store8_fast, load64_fast)
FUSE2(store16_fast, ospec_andi32)
FUSE2(ospec_orri32, store8_fast)
FUSE2(store16_rxl, ospec_addi64)
FUSE2(load32_fast, fused_cmpi32_eq)
FUSE2(ospec_addi64, ospec_ubfm64)
FUSE2(load64_fast, store32_fast)
FUSE2(ospec_subi32, ospec_subi64)
FUSE2(ospec_addi64, ospec_subr32)
FUSE2(ospec_ubfm32, store32_fast)
FUSE2(ospec_ubfm32, ospec_orri32)
FUSE2(vspec_umov32, load32_fast)
FUSE2(load32_fast, load32_rxl)
FUSE2(ospec_orri32, store16_fast)
FUSE2(ospec_addi64, fused_cmpr64_eq)
FUSE2(ospec_eorr32, b)
FUSE2(ospec_addi32, ospec_eorr32)
FUSE2(load8_rxl, ospec_addr_lsl32)
FUSE2(vspec_umov32, cbnz_w)
FUSE2(ospec_addi64, fused_cmpi64_hi)
FUSE2(ospec_andi32, store16_fast)
FUSE2(load32_fast, load64_rxl)
FUSE2(ospec_addi32, fused_cmpi32_hi)
FUSE2(ospec_subr32, ospec_orrr32)
FUSE2(ospec_orrr32, ospec_ubfm64)
FUSE2(load32_fast, load8_rxl)
FUSE2(vspec_umov32, fused_cmpr32_ls)
FUSE2(vspec_umov64, br_cached)
FUSE2(ospec_addr32, ospec_addr_lsl64)
FUSE2(ospec_ubfm64, ospec_ubfm64)
FUSE2(load32_fast, fused_cmpi32_le)
FUSE2(vspec_umov64, fused_cmpi32_hi)
FUSE2(vspec_umov64, store32_fast)
FUSE2(ospec_andr32, ospec_addi32)
FUSE2(ospec_ubfm64, ospec_addi64)
FUSE2(load64_fast, cbz_w)
FUSE2(ospec_subi64, fused_cmpi32_ne)
FUSE2(ospec_ubfm64, ospec_ubfm32)
FUSE2(ospec_addr64, load16_fast)
FUSE2(ospec_andr32, ospec_andr32)
FUSE2(load32_fast, ospec_subr32)
FUSE2(ospec_addr64, ospec_subr32)
FUSE2(ospec_addi32, ospec_addr_lsl64)
FUSE2(store64_fast, tb_nz)
FUSE2(ospec_subr64, mov_const)
FUSE2(ospec_ubfm32, ospec_addi32)
