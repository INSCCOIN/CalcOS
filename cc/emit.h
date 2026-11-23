/* Thumb-2 instruction emitter for the CalcOS on-device C compiler.
 *
 * Targets ARMv8-M mainline (Cortex-M33). Every encoding in emit.c is checked
 * byte-for-byte against GNU as by tests/test_encodings.py; if you add one,
 * add it to that table too.
 *
 * Design notes:
 *  - 32-bit constants are always built with movw+movt. That costs 8 bytes but
 *    removes literal pools entirely, which in a single-pass compiler is worth
 *    far more than the space.
 *  - Code is emitted into a buffer whose final address is known up front, so
 *    there is no relocation step: addresses are baked in as they are emitted.
 */
#ifndef CC_EMIT_H
#define CC_EMIT_H

#include <stdint.h>
#include <stddef.h>

/* registers */
enum {
    R0, R1, R2, R3, R4, R5, R6, R7,
    R8, R9, R10, R11, R12, SP, LR, PC
};

/* condition codes */
enum {
    CC_EQ, CC_NE, CC_CS, CC_CC, CC_MI, CC_PL, CC_VS, CC_VC,
    CC_HI, CC_LS, CC_GE, CC_LT, CC_GT, CC_LE, CC_AL
};

typedef struct {
    uint8_t  *buf;      /* output buffer                      */
    int       len;      /* bytes written                      */
    int       cap;      /* buffer capacity                    */
    uint32_t  base;     /* address buf[0] will live at        */
    int       overflow; /* set if we ran out of room          */
} Emit;

void emit_init(Emit *e, uint8_t *buf, int cap, uint32_t base);
int  emit_here(Emit *e);                 /* current offset            */
uint32_t emit_addr(Emit *e, int off);    /* runtime address of offset */

void e_hw(Emit *e, unsigned hw);         /* raw 16-bit halfword */
void e_w(Emit *e, unsigned hw1, unsigned hw2);

/* data movement */
void em_mov(Emit *e, int rd, int rm);
void em_movw(Emit *e, int rd, unsigned imm16);
void em_movt(Emit *e, int rd, unsigned imm16);
void em_imm32(Emit *e, int rd, uint32_t v);   /* movw [+ movt] */
void em_movs_imm8(Emit *e, int rd, unsigned imm8);

/* arithmetic / logic: rd = rn OP rm */
void em_add(Emit *e, int rd, int rn, int rm);
void em_sub(Emit *e, int rd, int rn, int rm);
void em_mul(Emit *e, int rd, int rn, int rm);
void em_sdiv(Emit *e, int rd, int rn, int rm);
void em_udiv(Emit *e, int rd, int rn, int rm);
void em_and(Emit *e, int rd, int rn, int rm);
void em_orr(Emit *e, int rd, int rn, int rm);
void em_eor(Emit *e, int rd, int rn, int rm);
void em_bic(Emit *e, int rd, int rn, int rm);
void em_lsl(Emit *e, int rd, int rn, int rm);
void em_lsr(Emit *e, int rd, int rn, int rm);
void em_asr(Emit *e, int rd, int rn, int rm);
void em_lsl_imm(Emit *e, int rd, int rm, int sh);
void em_lsr_imm(Emit *e, int rd, int rm, int sh);
void em_asr_imm(Emit *e, int rd, int rm, int sh);
void em_neg(Emit *e, int rd, int rn);
void em_mvn(Emit *e, int rd, int rm);
void em_add_imm(Emit *e, int rd, int rn, int imm);   /* 0..4095 */
void em_sub_imm(Emit *e, int rd, int rn, int imm);
/* Always the 4-byte ADDW/SUBW form, even for 0: use these wherever the
 * instruction will be patched later, so its size never changes. */
void em_addw_fixed(Emit *e, int rd, int rn, int imm);
void em_subw_fixed(Emit *e, int rd, int rn, int imm);

/* compare */
void em_cmp(Emit *e, int rn, int rm);
void em_cmp_imm(Emit *e, int rn, unsigned imm8);

/* memory: rt <-> [rn, #imm] */
void em_ldr(Emit *e, int rt, int rn, int imm);
void em_str(Emit *e, int rt, int rn, int imm);
void em_ldrb(Emit *e, int rt, int rn, int imm);
void em_strb(Emit *e, int rt, int rn, int imm);
void em_ldrh(Emit *e, int rt, int rn, int imm);
void em_strh(Emit *e, int rt, int rn, int imm);
void em_ldrsb(Emit *e, int rt, int rn, int imm);
void em_ldrsh(Emit *e, int rt, int rn, int imm);

/* sign/zero extension */
void em_sxtb(Emit *e, int rd, int rm);
void em_sxth(Emit *e, int rd, int rm);
void em_uxtb(Emit *e, int rd, int rm);
void em_uxth(Emit *e, int rd, int rm);

/* stack */
void em_push(Emit *e, unsigned mask);    /* bit n = register n */
void em_pop(Emit *e, unsigned mask);
void em_add_sp(Emit *e, int imm);        /* sp += imm (multiple of 4) */
void em_sub_sp(Emit *e, int imm);

/* control flow. Branch helpers return the offset of the instruction so it can
 * be patched once the destination is known. */
void em_bx(Emit *e, int rm);
void em_blx(Emit *e, int rm);
int  em_b(Emit *e, int target);          /* target = offset, or -1 to patch */
int  em_bcc(Emit *e, int cond, int target);
void em_bl_abs(Emit *e, uint32_t addr);  /* call an absolute address  */
void em_call_reg(Emit *e, uint32_t addr, int scratch);
void em_patch_branch(Emit *e, int at, int target);

#endif
