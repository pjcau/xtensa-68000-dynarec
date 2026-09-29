/* thumb.c - JIT step 4: a small Thumb (ARM7TDMI) translator on xjit.
 *
 * Subset: format 1 (LSL/LSR/ASR #imm), 2 (ADD/SUB reg/imm3), 3 (MOV/CMP/ADD/
 * SUB #imm8), 4 (the 16 ALU ops), 5 (hi-register ADD/CMP/MOV, no PC). The
 * reference interpreter follows gpSP's cpu.cpp exactly (thumb_add, thumb_sub,
 * thumb_logic, thumb_shift_*), quirks included: a register shift uses the
 * whole register, not its low byte; MUL sets N and Z only.
 *
 * Generated code: a2 = the guest state (r0..r15, then n, z, c, v as 0/1).
 * Every guest register lives in memory for now: correctness first. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "thumb.h"

#define OFF_R(n) (4 * (n))
#define OFF_N 64
#define OFF_Z 68
#define OFF_C 72
#define OFF_V 76

/* ---- reference ------------------------------------------------------------ */
static inline uint32_t ror32(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

static void nz(thumb_state_t *s, uint32_t d) { s->n = d >> 31; s->z = d == 0; }
static uint32_t add(thumb_state_t *s, uint32_t a, uint32_t b, uint32_t cin)
{
    uint32_t d = a + b;
    s->c = d < b;
    d += cin;
    s->c |= d < cin;
    nz(s, d);
    s->v = (~(a ^ b) & (a ^ d)) >> 31;
    return d;
}
static uint32_t sub(thumb_state_t *s, uint32_t a, uint32_t b, uint32_t cin)
{
    uint32_t d = a + ~b + cin;
    nz(s, d);
    s->c = cin ? b <= a : b < a;
    s->v = ((a ^ b) & (~b ^ d)) >> 31;
    return d;
}

void thumb_ref(thumb_state_t *s, const uint16_t *code, int count)
{
    for (int i = 0; i < count; i++)
    {
        uint32_t op = code[i], *r = s->r;
        uint32_t rd = op & 7, rs = (op >> 3) & 7, imm5 = (op >> 6) & 31;
        switch (op >> 11)
        {
        case 0x00: /* LSL #imm */
        {
            uint32_t d = r[rs];
            if (imm5) { s->c = (d >> (32 - imm5)) & 1; d <<= imm5; }
            nz(s, d); r[rd] = d;
            continue;
        }
        case 0x01: /* LSR #imm */
        {
            uint32_t d;
            if (!imm5) { d = 0; s->c = r[rs] >> 31; }
            else { d = r[rs]; s->c = (d >> (imm5 - 1)) & 1; d >>= imm5; }
            nz(s, d); r[rd] = d;
            continue;
        }
        case 0x02: /* ASR #imm */
        {
            uint32_t d;
            if (!imm5) { d = (uint32_t)((int32_t)r[rs] >> 31); s->c = d & 1; }
            else { d = r[rs]; s->c = (d >> (imm5 - 1)) & 1; d = (uint32_t)((int32_t)d >> imm5); }
            nz(s, d); r[rd] = d;
            continue;
        }
        case 0x03: /* ADD/SUB reg or imm3 */
        {
            uint32_t b = (op & 0x400) ? (op >> 6) & 7 : r[(op >> 6) & 7];
            r[rd] = (op & 0x200) ? sub(s, r[rs], b, 1) : add(s, r[rs], b, 0);
            continue;
        }
        case 0x04: { uint32_t d = op & 0xFF; nz(s, d); r[(op >> 8) & 7] = d; continue; }   /* MOV #imm8 */
        case 0x05: sub(s, r[(op >> 8) & 7], op & 0xFF, 1); continue;                        /* CMP #imm8 */
        case 0x06: r[(op >> 8) & 7] = add(s, r[(op >> 8) & 7], op & 0xFF, 0); continue;     /* ADD #imm8 */
        case 0x07: r[(op >> 8) & 7] = sub(s, r[(op >> 8) & 7], op & 0xFF, 1); continue;     /* SUB #imm8 */
        }
        if ((op >> 10) == 0x10) /* format 4 */
        {
            uint32_t a = r[rd], b = r[rs], d, sh = b;
            switch ((op >> 6) & 15)
            {
            case 0x0: d = a & b; nz(s, d); r[rd] = d; break;
            case 0x1: d = a ^ b; nz(s, d); r[rd] = d; break;
            case 0x2: /* LSL reg */
                d = a;
                if (sh) { if (sh > 31) { s->c = sh == 32 ? d & 1 : 0; d = 0; } else { s->c = (d >> (32 - sh)) & 1; d <<= sh; } }
                nz(s, d); r[rd] = d; break;
            case 0x3: /* LSR reg */
                d = a;
                if (sh) { if (sh > 31) { s->c = sh == 32 ? d >> 31 : 0; d = 0; } else { s->c = (d >> (sh - 1)) & 1; d >>= sh; } }
                nz(s, d); r[rd] = d; break;
            case 0x4: /* ASR reg */
                d = a;
                if (sh) { if (sh > 31) { d = (uint32_t)((int32_t)d >> 31); s->c = d & 1; } else { s->c = (d >> (sh - 1)) & 1; d = (uint32_t)((int32_t)d >> sh); } }
                nz(s, d); r[rd] = d; break;
            case 0x5: r[rd] = add(s, a, b, s->c); break;   /* ADC */
            case 0x6: r[rd] = sub(s, a, b, s->c); break;   /* SBC */
            case 0x7: /* ROR reg: shift counts use 5 bits, as on x86 and Xtensa */
                d = a;
                if (sh) { s->c = (d >> ((sh - 1) & 31)) & 1; d = ror32(d, sh); }
                nz(s, d); r[rd] = d; break;
            case 0x8: nz(s, a & b); break;                 /* TST */
            case 0x9: r[rd] = sub(s, 0, b, 1); break;      /* NEG */
            case 0xA: sub(s, a, b, 1); break;              /* CMP */
            case 0xB: add(s, a, b, 0); break;              /* CMN */
            case 0xC: d = a | b; nz(s, d); r[rd] = d; break;
            case 0xD: d = a * b; nz(s, d); r[rd] = d; break;   /* MUL: N, Z only */
            case 0xE: d = a & ~b; nz(s, d); r[rd] = d; break;
            case 0xF: d = ~b; nz(s, d); r[rd] = d; break;
            }
            continue;
        }
        if ((op >> 10) == 0x11) /* format 5, no PC */
        {
            uint32_t hs = (op >> 3) & 15, hd = ((op >> 4) & 8) | (op & 7);
            switch ((op >> 8) & 3)
            {
            case 0: r[hd] = r[hd] + r[hs]; break;
            case 1: sub(s, r[hd], r[hs], 1); break;
            case 2: r[hd] = r[hs]; break;
            }
        }
    }
}

/* ---- translator ----------------------------------------------------------- */
/* host registers: a2 state, a3..a13 temporaries */
enum { A = 3, B = 4, D = 5, T1 = 6, T2 = 7, T3 = 8, ONE = 9, CIN = 10, SH = 11 };

static void ld(xj_block_t *b, int h, int g) { xj_l32i(&b->e, h, 2, OFF_R(g)); }
static void st(xj_block_t *b, int g, int h) { xj_s32i(&b->e, h, 2, OFF_R(g)); }
static void st_flag(xj_block_t *b, int off, int h) { xj_s32i(&b->e, h, 2, off); }

static void emit_nz(xj_block_t *b, int d)
{
    xj_extui(&b->e, T1, d, 31, 1);
    st_flag(b, OFF_N, T1);
    xj_nsau(&b->e, T1, d);
    xj_extui(&b->e, T1, T1, 5, 1);
    st_flag(b, OFF_Z, T1);
}
/* D = A + B (+ CIN when adc): flags as gpSP's thumb_add */
static void emit_add(xj_block_t *b, bool adc)
{
    xj_emit_t *e = &b->e;
    xj_add(e, D, A, B);
    xj_saltu(e, T2, D, B);             /* c = d < b */
    if (adc)
    {
        xj_l32i(e, CIN, 2, OFF_C);
        xj_add(e, D, D, CIN);
        xj_saltu(e, T1, D, CIN);       /* c |= d < cin */
        xj_or(e, T2, T2, T1);
    }
    st_flag(b, OFF_C, T2);
    emit_nz(b, D);
    xj_xor(e, T1, A, D);               /* v = (~(a ^ b) & (a ^ d)) >> 31 = ((a ^ d) & (b ^ d)) >> 31 */
    xj_xor(e, T2, B, D);
    xj_and(e, T1, T1, T2);
    xj_extui(e, T1, T1, 31, 1);
    st_flag(b, OFF_V, T1);
}
/* D = A - B (sbc: A + ~B + CIN): flags as gpSP's thumb_sub */
static void emit_sub(xj_block_t *b, bool sbc)
{
    xj_emit_t *e = &b->e;
    xj_movi_n(e, ONE, 1);
    if (!sbc)
    {
        xj_sub(e, D, A, B);
        xj_saltu(e, T2, A, B);         /* c = b <= a = !(a < b) */
        xj_xor(e, T2, T2, ONE);
    }
    else
    {
        xj_l32i(e, CIN, 2, OFF_C);
        xj_sub(e, D, A, B);
        xj_add(e, D, D, CIN);
        xj_addi(e, D, D, -1);          /* a + ~b + cin = a - b - 1 + cin */
        xj_saltu(e, T2, A, B);         /* cin ? b <= a : b < a */
        xj_xor(e, T2, T2, ONE);
        xj_saltu(e, T3, B, A);
        xj_moveqz(e, T2, T3, CIN);
    }
    st_flag(b, OFF_C, T2);
    emit_nz(b, D);
    xj_xor(e, T1, A, B);               /* v = ((a ^ b) & ~(b ^ d)) >> 31 */
    xj_xor(e, T2, B, D);
    xj_movi(e, T3, -1);
    xj_xor(e, T2, T2, T3);
    xj_and(e, T1, T1, T2);
    xj_extui(e, T1, T1, 31, 1);
    st_flag(b, OFF_V, T1);
}
/* D = A >> sa (logical), sa 1..31 */
static void emit_srl_imm(xj_emit_t *e, int d, int a, int sa)
{
    if (sa <= 15) xj_srli(e, d, a, sa);
    else xj_extui(e, d, a, sa, 32 - sa);
}
/* T1 = (a >> amount_reg) & 1, amount taken mod 32 */
static void emit_bit_at(xj_emit_t *e, int a, int amount)
{
    xj_ssr(e, amount);
    xj_srl(e, T1, a);
    xj_extui(e, T1, T1, 0, 1);
}

static void emit_shift_reg(xj_block_t *b, int kind)
{
    xj_emit_t *e = &b->e;
    int done = xjb_label(b), big = xjb_label(b), store = xjb_label(b);
    /* A = value, SH = amount (the whole register, as gpSP) */
    xj_mov(e, D, A);
    xjb_bz(b, xj_beqz, SH, done);      /* 0: value and C unchanged */
    if (kind == 3) /* ROR */
    {
        xj_addi(e, T2, SH, -1);
        emit_bit_at(e, A, T2);
        st_flag(b, OFF_C, T1);
        xj_ssr(e, SH);
        xj_src(e, D, A, A);
        xjb_j(b, done);
    }
    else
    {
        xjb_bi(b, (void (*)(xj_emit_t *, int, int, int))xj_bgeui, SH, 32, big);
        switch (kind)
        {
        case 0: /* LSL: c = bit 32 - sh */
            xj_movi(e, T2, 32);
            xj_sub(e, T2, T2, SH);
            emit_bit_at(e, A, T2);
            xj_ssl(e, SH);
            xj_sll(e, D, A);
            break;
        case 1: /* LSR: c = bit sh - 1 */
        case 2: /* ASR */
            xj_addi(e, T2, SH, -1);
            emit_bit_at(e, A, T2);
            xj_ssr(e, SH);
            if (kind == 1) xj_srl(e, D, A); else xj_sra(e, D, A);
            break;
        }
        xjb_j(b, store);
        xjb_bind(b, big);              /* sh >= 32 */
        switch (kind)
        {
        case 0: /* c = sh == 32 ? d & 1 : 0; d = 0 */
        case 1: /* c = sh == 32 ? d >> 31 : 0; d = 0 */
            xj_movi(e, T1, 0);
            xj_movi(e, D, 0);
            {
                int not32 = xjb_label(b);
                xjb_bi(b, xj_bnei, SH, 32, not32);
                xj_extui(e, T1, A, kind == 0 ? 0 : 31, 1);
                xjb_bind(b, not32);
            }
            break;
        case 2: /* d = sign; c = d & 1 */
            xj_srai(e, D, A, 31);
            xj_extui(e, T1, D, 0, 1);
            break;
        }
        xjb_bind(b, store);
        st_flag(b, OFF_C, T1);
    }
    xjb_bind(b, done);
}

bool thumb_translate(xj_block_t *b, const uint16_t *code, int count)
{
    xj_emit_t *e = &b->e;
    xj_entry(e, 1, 32);
    for (int i = 0; i < count; i++)
    {
        uint32_t op = code[i];
        int rd = op & 7, rs = (op >> 3) & 7, imm5 = (op >> 6) & 31;
        switch (op >> 11)
        {
        case 0x00: /* LSL #imm */
            ld(b, A, rs);
            if (imm5)
            {
                xj_extui(e, T1, A, 32 - imm5, 1);
                st_flag(b, OFF_C, T1);
                xj_slli(e, D, A, imm5);
            }
            else
                xj_mov(e, D, A);
            emit_nz(b, D); st(b, rd, D);
            continue;
        case 0x01: /* LSR #imm */
            ld(b, A, rs);
            if (!imm5) { xj_extui(e, T1, A, 31, 1); xj_movi(e, D, 0); }
            else { xj_extui(e, T1, A, imm5 - 1, 1); emit_srl_imm(e, D, A, imm5); }
            st_flag(b, OFF_C, T1);
            emit_nz(b, D); st(b, rd, D);
            continue;
        case 0x02: /* ASR #imm */
            ld(b, A, rs);
            if (!imm5) { xj_srai(e, D, A, 31); xj_extui(e, T1, D, 0, 1); }
            else { xj_extui(e, T1, A, imm5 - 1, 1); xj_srai(e, D, A, imm5); }
            st_flag(b, OFF_C, T1);
            emit_nz(b, D); st(b, rd, D);
            continue;
        case 0x03: /* ADD/SUB reg or imm3 */
            ld(b, A, rs);
            if (op & 0x400) xj_movi(e, B, (op >> 6) & 7);
            else ld(b, B, (op >> 6) & 7);
            if (op & 0x200) emit_sub(b, false); else emit_add(b, false);
            st(b, rd, D);
            continue;
        case 0x04: /* MOV #imm8 */
            xj_movi(e, D, op & 0xFF);
            emit_nz(b, D); st(b, (op >> 8) & 7, D);
            continue;
        case 0x05: case 0x06: case 0x07: /* CMP/ADD/SUB #imm8 */
            ld(b, A, (op >> 8) & 7);
            xj_movi(e, B, op & 0xFF);
            if ((op >> 11) == 0x06) emit_add(b, false); else emit_sub(b, false);
            if ((op >> 11) != 0x05) st(b, (op >> 8) & 7, D);
            continue;
        }
        if ((op >> 10) == 0x10) /* format 4 */
        {
            ld(b, A, rd);
            ld(b, B, rs);
            switch ((op >> 6) & 15)
            {
            case 0x0: xj_and(e, D, A, B); emit_nz(b, D); st(b, rd, D); break;
            case 0x1: xj_xor(e, D, A, B); emit_nz(b, D); st(b, rd, D); break;
            case 0x2: case 0x3: case 0x4: case 0x7:
                xj_mov(e, SH, B);
                emit_shift_reg(b, ((op >> 6) & 15) == 0x7 ? 3 : ((op >> 6) & 15) - 2);
                emit_nz(b, D); st(b, rd, D);
                break;
            case 0x5: emit_add(b, true); st(b, rd, D); break;
            case 0x6: emit_sub(b, true); st(b, rd, D); break;
            case 0x8: xj_and(e, D, A, B); emit_nz(b, D); break;
            case 0x9: xj_movi(e, A, 0); emit_sub(b, false); st(b, rd, D); break;
            case 0xA: emit_sub(b, false); break;
            case 0xB: emit_add(b, false); break;
            case 0xC: xj_or(e, D, A, B); emit_nz(b, D); st(b, rd, D); break;
            case 0xD: xj_mull(e, D, A, B); emit_nz(b, D); st(b, rd, D); break;
            case 0xE: xj_movi(e, T3, -1); xj_xor(e, T3, B, T3); xj_and(e, D, A, T3); emit_nz(b, D); st(b, rd, D); break;
            case 0xF: xj_movi(e, T3, -1); xj_xor(e, D, B, T3); emit_nz(b, D); st(b, rd, D); break;
            }
            continue;
        }
        if ((op >> 10) == 0x11) /* format 5 */
        {
            int hs = (op >> 3) & 15, hd = ((op >> 4) & 8) | (op & 7);
            if (hs == 15 || hd == 15)
                return false;
            switch ((op >> 8) & 3)
            {
            case 0: ld(b, A, hd); ld(b, B, hs); xj_add(e, D, A, B); st(b, hd, D); break;
            case 1: ld(b, A, hd); ld(b, B, hs); emit_sub(b, false); break;
            case 2: ld(b, D, hs); st(b, hd, D); break;
            default: return false;   /* BX */
            }
            continue;
        }
        return false;   /* not in the subset */
    }
    xj_retw_n(e);
    return xjb_ok(b);
}

/* ---- random programs ------------------------------------------------------- */
static uint32_t rs_state = 0xC0FFEE;
static uint32_t rnd(void) { rs_state ^= rs_state << 13; rs_state ^= rs_state >> 17; rs_state ^= rs_state << 5; return rs_state; }

uint16_t thumb_random_op(void)
{
    for (;;)
    {
        uint32_t k = rnd() % 5, op;
        switch (k)
        {
        case 0: op = ((rnd() % 3) << 11) | (rnd() & 0x7FF); break;                 /* format 1 */
        case 1: op = 0x1800 | (rnd() & 0x7FF); break;                               /* format 2 */
        case 2: op = 0x2000 | (rnd() & 0x1FFF); break;                              /* format 3 */
        case 3: op = 0x4000 | (rnd() & 0x3FF); break;                               /* format 4 */
        default: op = 0x4400 | (rnd() & 0x3FF);                                     /* format 5 */
            if (((op >> 8) & 3) == 3 || ((op >> 3) & 15) == 15 || ((((op >> 4) & 8) | (op & 7)) == 15)) continue;
            break;
        }
        return (uint16_t)op;
    }
}

static uint32_t interesting(void)
{
    static const uint32_t v[] = {0, 1, 2, 31, 32, 33, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 255, 256, 0x80000001};
    switch (rnd() % 4)
    {
    case 0: return v[rnd() % (sizeof(v) / sizeof(v[0]))];
    case 1: return rnd() % 40;          /* shift amounts around 32 */
    default: return rnd();
    }
}

void thumb_random_state(thumb_state_t *s)
{
    for (int i = 0; i < 16; i++)
        s->r[i] = interesting();
    s->n = rnd() & 1; s->z = rnd() & 1; s->c = rnd() & 1; s->v = rnd() & 1;
}
