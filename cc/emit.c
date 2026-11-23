/* Thumb-2 encoder. See emit.h. Encodings verified against arm-none-eabi-as. */
#include "emit.h"

#define LOW(r) ((r) < 8)

void emit_init(Emit *e, uint8_t *buf, int cap, uint32_t base)
{
    e->buf = buf;
    e->cap = cap;
    e->len = 0;
    e->base = base;
    e->overflow = 0;
}

int emit_here(Emit *e) { return e->len; }
uint32_t emit_addr(Emit *e, int off) { return e->base + (uint32_t)off; }

void e_hw(Emit *e, unsigned hw)
{
    if (e->len + 2 > e->cap) { e->overflow = 1; return; }
    e->buf[e->len++] = (uint8_t)(hw & 0xFF);
    e->buf[e->len++] = (uint8_t)((hw >> 8) & 0xFF);
}

void e_w(Emit *e, unsigned hw1, unsigned hw2) { e_hw(e, hw1); e_hw(e, hw2); }

/* ------------------------------------------------------------------ */
/* data movement                                                       */
/* ------------------------------------------------------------------ */

void em_mov(Emit *e, int rd, int rm)
{
    if (rd == rm) return;
    /* MOV (register) T1: handles high registers too */
    e_hw(e, 0x4600u | ((unsigned)(rd & 8) << 4) | ((unsigned)rm << 3)
             | (unsigned)(rd & 7));
}

void em_movs_imm8(Emit *e, int rd, unsigned imm8)
{
    e_hw(e, 0x2000u | ((unsigned)rd << 8) | (imm8 & 0xFF));
}

/* MOVW T3: 11110 i 100100 imm4 : 0 imm3 rd imm8 */
void em_movw(Emit *e, int rd, unsigned imm16)
{
    unsigned i    = (imm16 >> 11) & 1;
    unsigned imm4 = (imm16 >> 12) & 0xF;
    unsigned imm3 = (imm16 >> 8) & 7;
    unsigned imm8 = imm16 & 0xFF;
    e_w(e, 0xF240u | (i << 10) | imm4,
           (imm3 << 12) | ((unsigned)rd << 8) | imm8);
}

/* MOVT T1: 11110 i 101100 imm4 : 0 imm3 rd imm8 */
void em_movt(Emit *e, int rd, unsigned imm16)
{
    unsigned i    = (imm16 >> 11) & 1;
    unsigned imm4 = (imm16 >> 12) & 0xF;
    unsigned imm3 = (imm16 >> 8) & 7;
    unsigned imm8 = imm16 & 0xFF;
    e_w(e, 0xF2C0u | (i << 10) | imm4,
           (imm3 << 12) | ((unsigned)rd << 8) | imm8);
}

void em_imm32(Emit *e, int rd, uint32_t v)
{
    if (v < 256 && LOW(rd)) { em_movs_imm8(e, rd, v); return; }
    em_movw(e, rd, v & 0xFFFF);
    if (v >> 16) em_movt(e, rd, (v >> 16) & 0xFFFF);
}

/* ------------------------------------------------------------------ */
/* arithmetic                                                          */
/* ------------------------------------------------------------------ */

void em_add(Emit *e, int rd, int rn, int rm)
{
    if (LOW(rd) && LOW(rn) && LOW(rm)) {
        e_hw(e, 0x1800u | ((unsigned)rm << 6) | ((unsigned)rn << 3) | (unsigned)rd);
        return;
    }
    e_w(e, 0xEB00u | (unsigned)rn, ((unsigned)rd << 8) | (unsigned)rm);
}

void em_sub(Emit *e, int rd, int rn, int rm)
{
    if (LOW(rd) && LOW(rn) && LOW(rm)) {
        e_hw(e, 0x1A00u | ((unsigned)rm << 6) | ((unsigned)rn << 3) | (unsigned)rd);
        return;
    }
    e_w(e, 0xEBA0u | (unsigned)rn, ((unsigned)rd << 8) | (unsigned)rm);
}

/* MUL T2 (32-bit form, no flags) */
void em_mul(Emit *e, int rd, int rn, int rm)
{
    e_w(e, 0xFB00u | (unsigned)rn, 0xF000u | ((unsigned)rd << 8) | (unsigned)rm);
}

void em_sdiv(Emit *e, int rd, int rn, int rm)
{
    e_w(e, 0xFB90u | (unsigned)rn, 0xF0F0u | ((unsigned)rd << 8) | (unsigned)rm);
}

void em_udiv(Emit *e, int rd, int rn, int rm)
{
    e_w(e, 0xFBB0u | (unsigned)rn, 0xF0F0u | ((unsigned)rd << 8) | (unsigned)rm);
}

static void logic_w(Emit *e, unsigned op, int rd, int rn, int rm)
{
    e_w(e, op | (unsigned)rn, ((unsigned)rd << 8) | (unsigned)rm);
}

void em_and(Emit *e, int rd, int rn, int rm) { logic_w(e, 0xEA00u, rd, rn, rm); }
void em_orr(Emit *e, int rd, int rn, int rm) { logic_w(e, 0xEA40u, rd, rn, rm); }
void em_eor(Emit *e, int rd, int rn, int rm) { logic_w(e, 0xEA80u, rd, rn, rm); }
void em_bic(Emit *e, int rd, int rn, int rm) { logic_w(e, 0xEA20u, rd, rn, rm); }

static void shift_w(Emit *e, unsigned op, int rd, int rn, int rm)
{
    e_w(e, op | (unsigned)rn, 0xF000u | ((unsigned)rd << 8) | (unsigned)rm);
}

void em_lsl(Emit *e, int rd, int rn, int rm) { shift_w(e, 0xFA00u, rd, rn, rm); }
void em_lsr(Emit *e, int rd, int rn, int rm) { shift_w(e, 0xFA20u, rd, rn, rm); }
void em_asr(Emit *e, int rd, int rn, int rm) { shift_w(e, 0xFA40u, rd, rn, rm); }

/* LSL/LSR/ASR immediate, 32-bit form so high registers work */
static void shimm_w(Emit *e, unsigned type, int rd, int rm, int sh)
{
    unsigned imm3 = ((unsigned)sh >> 2) & 7;
    unsigned imm2 = (unsigned)sh & 3;
    e_w(e, 0xEA4Fu,
        (imm3 << 12) | ((unsigned)rd << 8) | (imm2 << 6) | (type << 4) | (unsigned)rm);
}

void em_lsl_imm(Emit *e, int rd, int rm, int sh)
{
    if (sh == 0) { em_mov(e, rd, rm); return; }
    shimm_w(e, 0, rd, rm, sh);
}
void em_lsr_imm(Emit *e, int rd, int rm, int sh)
{
    if (sh == 0) { em_mov(e, rd, rm); return; }
    shimm_w(e, 1, rd, rm, sh & 31);
}
void em_asr_imm(Emit *e, int rd, int rm, int sh)
{
    if (sh == 0) { em_mov(e, rd, rm); return; }
    shimm_w(e, 2, rd, rm, sh & 31);
}

/* RSB rd, rn, #0 */
void em_neg(Emit *e, int rd, int rn)
{
    e_w(e, 0xF1C0u | (unsigned)rn, ((unsigned)rd << 8));
}

void em_mvn(Emit *e, int rd, int rm)
{
    e_w(e, 0xEA6Fu, ((unsigned)rd << 8) | (unsigned)rm);
}

static void addsub_w(Emit *e, unsigned op, int rd, int rn, int imm)
{
    unsigned u = (unsigned)imm & 0xFFF;
    unsigned i = (u >> 11) & 1, imm3 = (u >> 8) & 7, imm8 = u & 0xFF;
    e_w(e, op | (i << 10) | (unsigned)rn,
           (imm3 << 12) | ((unsigned)rd << 8) | imm8);
}

void em_addw_fixed(Emit *e, int rd, int rn, int imm)
{ addsub_w(e, 0xF200u, rd, rn, imm); }

void em_subw_fixed(Emit *e, int rd, int rn, int imm)
{ addsub_w(e, 0xF2A0u, rd, rn, imm); }

/* ADD/SUB immediate, 0..4095, via ADDW/SUBW (no flags, any register) */
void em_add_imm(Emit *e, int rd, int rn, int imm)
{
    unsigned u, i, imm3, imm8;
    if (imm < 0) { em_sub_imm(e, rd, rn, -imm); return; }
    if (imm == 0) { em_mov(e, rd, rn); return; }
    u = (unsigned)imm & 0xFFF;
    i = (u >> 11) & 1;
    imm3 = (u >> 8) & 7;
    imm8 = u & 0xFF;
    e_w(e, 0xF200u | (i << 10) | (unsigned)rn,
           (imm3 << 12) | ((unsigned)rd << 8) | imm8);
}

void em_sub_imm(Emit *e, int rd, int rn, int imm)
{
    unsigned u, i, imm3, imm8;
    if (imm < 0) { em_add_imm(e, rd, rn, -imm); return; }
    if (imm == 0) { em_mov(e, rd, rn); return; }
    u = (unsigned)imm & 0xFFF;
    i = (u >> 11) & 1;
    imm3 = (u >> 8) & 7;
    imm8 = u & 0xFF;
    e_w(e, 0xF2A0u | (i << 10) | (unsigned)rn,
           (imm3 << 12) | ((unsigned)rd << 8) | imm8);
}

/* ------------------------------------------------------------------ */
/* compare                                                             */
/* ------------------------------------------------------------------ */

void em_cmp(Emit *e, int rn, int rm)
{
    if (LOW(rn) && LOW(rm)) {
        e_hw(e, 0x4280u | ((unsigned)rm << 3) | (unsigned)rn);
        return;
    }
    e_w(e, 0xEBB0u | (unsigned)rn, 0x0F00u | (unsigned)rm);
}

void em_cmp_imm(Emit *e, int rn, unsigned imm8)
{
    if (LOW(rn) && imm8 < 256) {
        e_hw(e, 0x2800u | ((unsigned)rn << 8) | (imm8 & 0xFF));
        return;
    }
    e_w(e, 0xF1B0u | (unsigned)rn, 0x0F00u | (imm8 & 0xFF));
}

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */

/* All loads/stores use the wide form with a 12-bit positive offset, except
 * the common low-register short forms which save two bytes. */
static void mem_w(Emit *e, unsigned op, int rt, int rn, int imm)
{
    e_w(e, op | (unsigned)rn, ((unsigned)rt << 12) | ((unsigned)imm & 0xFFF));
}

void em_ldr(Emit *e, int rt, int rn, int imm)
{
    if (LOW(rt) && LOW(rn) && imm >= 0 && imm < 128 && (imm & 3) == 0) {
        e_hw(e, 0x6800u | (((unsigned)imm >> 2) << 6) | ((unsigned)rn << 3) | (unsigned)rt);
        return;
    }
    if (rn == SP && LOW(rt) && imm >= 0 && imm < 1024 && (imm & 3) == 0) {
        e_hw(e, 0x9800u | ((unsigned)rt << 8) | ((unsigned)imm >> 2));
        return;
    }
    mem_w(e, 0xF8D0u, rt, rn, imm);
}

void em_str(Emit *e, int rt, int rn, int imm)
{
    if (LOW(rt) && LOW(rn) && imm >= 0 && imm < 128 && (imm & 3) == 0) {
        e_hw(e, 0x6000u | (((unsigned)imm >> 2) << 6) | ((unsigned)rn << 3) | (unsigned)rt);
        return;
    }
    if (rn == SP && LOW(rt) && imm >= 0 && imm < 1024 && (imm & 3) == 0) {
        e_hw(e, 0x9000u | ((unsigned)rt << 8) | ((unsigned)imm >> 2));
        return;
    }
    mem_w(e, 0xF8C0u, rt, rn, imm);
}

void em_ldrb(Emit *e, int rt, int rn, int imm)
{
    if (LOW(rt) && LOW(rn) && imm >= 0 && imm < 32) {
        e_hw(e, 0x7800u | ((unsigned)imm << 6) | ((unsigned)rn << 3) | (unsigned)rt);
        return;
    }
    mem_w(e, 0xF890u, rt, rn, imm);
}

void em_strb(Emit *e, int rt, int rn, int imm)
{
    if (LOW(rt) && LOW(rn) && imm >= 0 && imm < 32) {
        e_hw(e, 0x7000u | ((unsigned)imm << 6) | ((unsigned)rn << 3) | (unsigned)rt);
        return;
    }
    mem_w(e, 0xF880u, rt, rn, imm);
}

void em_ldrh(Emit *e, int rt, int rn, int imm)
{
    if (LOW(rt) && LOW(rn) && imm >= 0 && imm < 64 && (imm & 1) == 0) {
        e_hw(e, 0x8800u | (((unsigned)imm >> 1) << 6) | ((unsigned)rn << 3) | (unsigned)rt);
        return;
    }
    mem_w(e, 0xF8B0u, rt, rn, imm);
}

void em_strh(Emit *e, int rt, int rn, int imm)
{
    if (LOW(rt) && LOW(rn) && imm >= 0 && imm < 64 && (imm & 1) == 0) {
        e_hw(e, 0x8000u | (((unsigned)imm >> 1) << 6) | ((unsigned)rn << 3) | (unsigned)rt);
        return;
    }
    mem_w(e, 0xF8A0u, rt, rn, imm);
}

void em_ldrsb(Emit *e, int rt, int rn, int imm) { mem_w(e, 0xF990u, rt, rn, imm); }
void em_ldrsh(Emit *e, int rt, int rn, int imm) { mem_w(e, 0xF9B0u, rt, rn, imm); }

void em_sxtb(Emit *e, int rd, int rm)
{
    if (LOW(rd) && LOW(rm)) { e_hw(e, 0xB240u | ((unsigned)rm << 3) | (unsigned)rd); return; }
    e_w(e, 0xFA4Fu, 0xF080u | ((unsigned)rd << 8) | (unsigned)rm);
}
void em_sxth(Emit *e, int rd, int rm)
{
    if (LOW(rd) && LOW(rm)) { e_hw(e, 0xB200u | ((unsigned)rm << 3) | (unsigned)rd); return; }
    e_w(e, 0xFA0Fu, 0xF080u | ((unsigned)rd << 8) | (unsigned)rm);
}
void em_uxtb(Emit *e, int rd, int rm)
{
    if (LOW(rd) && LOW(rm)) { e_hw(e, 0xB2C0u | ((unsigned)rm << 3) | (unsigned)rd); return; }
    e_w(e, 0xFA5Fu, 0xF080u | ((unsigned)rd << 8) | (unsigned)rm);
}
void em_uxth(Emit *e, int rd, int rm)
{
    if (LOW(rd) && LOW(rm)) { e_hw(e, 0xB280u | ((unsigned)rm << 3) | (unsigned)rd); return; }
    e_w(e, 0xFA1Fu, 0xF080u | ((unsigned)rd << 8) | (unsigned)rm);
}

/* ------------------------------------------------------------------ */
/* stack                                                               */
/* ------------------------------------------------------------------ */

void em_push(Emit *e, unsigned mask)
{
    if ((mask & ~0x40FFu) == 0) {           /* r0-r7 and lr */
        e_hw(e, 0xB400u | ((mask & 0x4000u) ? 0x100u : 0u) | (mask & 0xFF));
        return;
    }
    e_w(e, 0xE92Du, mask & 0x5FFFu);
}

void em_pop(Emit *e, unsigned mask)
{
    if ((mask & ~0x80FFu) == 0) {           /* r0-r7 and pc */
        e_hw(e, 0xBC00u | ((mask & 0x8000u) ? 0x100u : 0u) | (mask & 0xFF));
        return;
    }
    e_w(e, 0xE8BDu, mask & 0xDFFFu);
}

void em_add_sp(Emit *e, int imm)
{
    if (imm == 0) return;
    if (imm > 0 && imm < 512 && (imm & 3) == 0) {
        e_hw(e, 0xB000u | ((unsigned)imm >> 2));
        return;
    }
    em_add_imm(e, SP, SP, imm);
}

void em_sub_sp(Emit *e, int imm)
{
    if (imm == 0) return;
    if (imm > 0 && imm < 512 && (imm & 3) == 0) {
        e_hw(e, 0xB080u | ((unsigned)imm >> 2));
        return;
    }
    em_sub_imm(e, SP, SP, imm);
}

/* ------------------------------------------------------------------ */
/* control flow                                                        */
/* ------------------------------------------------------------------ */

void em_bx(Emit *e, int rm)  { e_hw(e, 0x4700u | ((unsigned)rm << 3)); }
void em_blx(Emit *e, int rm) { e_hw(e, 0x4780u | ((unsigned)rm << 3)); }

/* B.W (T4). Always the wide form so the range is never a surprise and the
 * instruction size does not change when a branch is patched. */
static void enc_b_t4(Emit *e, int at, int target)
{
    int32_t off = target - (at + 4);
    unsigned s, i1, i2, j1, j2, imm10, imm11;
    uint32_t u = (uint32_t)off >> 1;
    s     = (uint32_t)off < 0x80000000u ? ((off < 0) ? 1u : 0u) : 1u;
    s     = (off < 0) ? 1u : 0u;
    imm11 = u & 0x7FF;
    imm10 = (u >> 11) & 0x3FF;
    i1    = (u >> 21) & 1;
    i2    = (u >> 22) & 1;
    /* I1 = NOT(J1 EOR S)  =>  J1 = NOT(I1) EOR S */
    j1 = (~i1 & 1) ^ s;
    j2 = (~i2 & 1) ^ s;
    e->buf[at + 0] = (uint8_t)((0xF000u | (s << 10) | imm10) & 0xFF);
    e->buf[at + 1] = (uint8_t)(((0xF000u | (s << 10) | imm10) >> 8) & 0xFF);
    e->buf[at + 2] = (uint8_t)((0x9000u | (j1 << 13) | (j2 << 11) | imm11) & 0xFF);
    e->buf[at + 3] = (uint8_t)(((0x9000u | (j1 << 13) | (j2 << 11) | imm11) >> 8) & 0xFF);
}

/* B<cond>.W (T3) */
static void enc_bcc_t3(Emit *e, int at, int cond, int target)
{
    int32_t off = target - (at + 4);
    uint32_t u = (uint32_t)off >> 1;
    unsigned s = (off < 0) ? 1u : 0u;
    unsigned imm11 = u & 0x7FF;
    unsigned imm6  = (u >> 11) & 0x3F;
    unsigned j1    = (u >> 17) & 1;
    unsigned j2    = (u >> 18) & 1;
    unsigned hw1 = 0xF000u | (s << 10) | ((unsigned)cond << 6) | imm6;
    unsigned hw2 = 0x8000u | (j1 << 13) | (j2 << 11) | imm11;
    e->buf[at + 0] = (uint8_t)(hw1 & 0xFF);
    e->buf[at + 1] = (uint8_t)((hw1 >> 8) & 0xFF);
    e->buf[at + 2] = (uint8_t)(hw2 & 0xFF);
    e->buf[at + 3] = (uint8_t)((hw2 >> 8) & 0xFF);
}

int em_b(Emit *e, int target)
{
    int at = e->len;
    e_hw(e, 0); e_hw(e, 0);
    if (!e->overflow) enc_b_t4(e, at, target < 0 ? at : target);
    return at;
}

int em_bcc(Emit *e, int cond, int target)
{
    int at = e->len;
    e_hw(e, 0); e_hw(e, 0);
    if (!e->overflow) enc_bcc_t3(e, at, cond, target < 0 ? at : target);
    return at;
}

/* Patch a branch previously emitted at 'at' to jump to 'target'. The encoding
 * kind is recovered from the second halfword's top bits. */
void em_patch_branch(Emit *e, int at, int target)
{
    unsigned hw2;
    if (at < 0 || at + 4 > e->len) return;
    hw2 = (unsigned)e->buf[at + 2] | ((unsigned)e->buf[at + 3] << 8);
    if ((hw2 & 0xD000u) == 0x9000u) enc_b_t4(e, at, target);
    else {
        unsigned hw1 = (unsigned)e->buf[at] | ((unsigned)e->buf[at + 1] << 8);
        enc_bcc_t3(e, at, (int)((hw1 >> 6) & 0xF), target);
    }
}

/* Call an absolute address. BL has only +-16MB of range and firmware entry
 * points can be anywhere, so load the address and BLX it. */
void em_call_reg(Emit *e, uint32_t addr, int scratch)
{
    em_imm32(e, scratch, addr | 1u);      /* Thumb bit */
    em_blx(e, scratch);
}

void em_bl_abs(Emit *e, uint32_t addr) { em_call_reg(e, addr, R12); }
