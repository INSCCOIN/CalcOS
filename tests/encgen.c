/* Emits one test instruction per line as hex, to be diffed against GNU as.
 * The matching assembly text lives in tests/test_encodings.py, in the same
 * order. Keep the two lists in step. */
#include "emit.h"
#include <stdio.h>

static uint8_t buf[64];
static Emit e;

static void line(void)
{
    int i;
    for (i = 0; i < e.len; i++) printf("%02x", e.buf[i]);
    printf("\n");
    e.len = 0;
}

#define T(call) do { call; line(); } while (0)

int main(void)
{
    emit_init(&e, buf, sizeof buf, 0x20000000);

    /* moves */
    T(em_mov(&e, R1, R2));
    T(em_mov(&e, R8, R3));
    T(em_mov(&e, R4, R11));
    T(em_movs_imm8(&e, R0, 7));
    T(em_movs_imm8(&e, R5, 255));
    T(em_movw(&e, R0, 0x1234));
    T(em_movw(&e, R9, 0xBEEF));
    T(em_movt(&e, R0, 0x1234));
    T(em_movt(&e, R7, 0xFFFF));

    /* arithmetic */
    T(em_add(&e, R0, R1, R2));
    T(em_add(&e, R8, R1, R2));
    T(em_sub(&e, R0, R1, R2));
    T(em_sub(&e, R10, R11, R12));
    T(em_mul(&e, R0, R1, R2));
    T(em_mul(&e, R9, R10, R11));
    T(em_sdiv(&e, R0, R1, R2));
    T(em_udiv(&e, R3, R4, R5));
    T(em_and(&e, R0, R1, R2));
    T(em_orr(&e, R3, R4, R5));
    T(em_eor(&e, R6, R7, R8));
    T(em_bic(&e, R1, R2, R3));
    T(em_lsl(&e, R0, R1, R2));
    T(em_lsr(&e, R3, R4, R5));
    T(em_asr(&e, R6, R7, R8));
    T(em_lsl_imm(&e, R0, R1, 5));
    T(em_lsr_imm(&e, R2, R3, 17));
    T(em_asr_imm(&e, R4, R5, 31));
    T(em_neg(&e, R0, R1));
    T(em_neg(&e, R9, R10));
    T(em_mvn(&e, R0, R1));
    T(em_add_imm(&e, R0, R1, 100));
    T(em_add_imm(&e, R2, R3, 4000));
    T(em_sub_imm(&e, R4, R5, 1));
    T(em_sub_imm(&e, R6, R7, 2048));

    /* compare */
    T(em_cmp(&e, R0, R1));
    T(em_cmp(&e, R8, R9));
    T(em_cmp_imm(&e, R0, 10));
    T(em_cmp_imm(&e, R9, 10));

    /* memory */
    T(em_ldr(&e, R0, R1, 0));
    T(em_ldr(&e, R2, R3, 124));
    T(em_ldr(&e, R0, R1, 200));
    T(em_ldr(&e, R9, R10, 8));
    T(em_str(&e, R0, R1, 0));
    T(em_str(&e, R2, R3, 124));
    T(em_str(&e, R0, R1, 300));
    T(em_ldrb(&e, R0, R1, 3));
    T(em_ldrb(&e, R0, R1, 100));
    T(em_strb(&e, R2, R3, 31));
    T(em_ldrh(&e, R0, R1, 4));
    T(em_ldrh(&e, R0, R1, 100));
    T(em_strh(&e, R2, R3, 62));
    T(em_ldrsb(&e, R0, R1, 5));
    T(em_ldrsh(&e, R2, R3, 6));
    T(em_ldr(&e, R0, SP, 16));
    T(em_str(&e, R1, SP, 1020));
    T(em_ldr(&e, R9, SP, 16));

    /* extension */
    T(em_sxtb(&e, R0, R1));
    T(em_sxth(&e, R2, R3));
    T(em_uxtb(&e, R4, R5));
    T(em_uxth(&e, R6, R7));
    T(em_sxtb(&e, R8, R9));
    T(em_uxth(&e, R10, R11));

    /* stack */
    T(em_push(&e, (1u << R4) | (1u << R5) | (1u << LR)));
    T(em_push(&e, (1u << R4) | (1u << R8)));
    T(em_pop(&e, (1u << R4) | (1u << R5) | (1u << PC)));
    T(em_pop(&e, (1u << R4) | (1u << R8)));
    T(em_add_sp(&e, 16));
    T(em_sub_sp(&e, 16));
    T(em_sub_sp(&e, 1024));

    /* control flow */
    T(em_bx(&e, LR));
    T(em_blx(&e, R12));
    T(em_b(&e, 0));                 /* backward to 0 from offset 0 */
    T(em_bcc(&e, CC_EQ, 0));
    T(em_bcc(&e, CC_LT, 0));

    return 0;
}
