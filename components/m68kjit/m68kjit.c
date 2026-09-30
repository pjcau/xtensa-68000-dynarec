/* m68kjit.c - block cache, translator front half, backends and the dispatcher.
 *
 * A block is a run of instructions from one PC, in fixed code only (ROM:
 * code in RAM is interpreted, so a write never invalidates a block). It
 * ends after an instruction that always leaves (BRA, JMP, RTS, ...) or at
 * M68KJIT_MAX_INSNS. Running a block does, per instruction,
 *
 *     PPC = pc; PC = pc + 2; IR = op; handler(); cycles -= cyc[op];
 *     if (PC != next || cycles <= 0) leave;
 *
 * which is the interpreter's loop body with the fetch and decode done once.
 * Two backends: portable C (the model; the host fuzz test runs it) and, on
 * Xtensa, native code generated with xjit that does the same.
 */
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "m68kjit.h"
#ifdef M68KJIT_TRACE
#include <stdio.h>
#endif

#ifdef __XTENSA__
#define M68KJIT_XTENSA 1
#include "xjit_block.h"
#include "xjit_exec.h"
#endif

#define M68KJIT_MAX_INSNS 64
#define M68KJIT_MAX_BLOCKS 4096
#define M68KJIT_MAX_TOTAL (M68KJIT_MAX_BLOCKS * 16)     /* C backend: instructions over all blocks */
#define M68KJIT_CODE_SIZE (1024 * 1024)                 /* Xtensa backend: code cache */
#define HASH_BITS 12
#define HASH(pc) ((((pc) >> 1) * 2654435761u) >> (32 - HASH_BITS))

typedef struct
{
    m68kjit_handler_t handler;
    uint32_t pc, next;
    uint16_t op;
    uint8_t cyc;
} insn_t;

typedef struct
{
    uint32_t pc;
#ifdef M68KJIT_XTENSA
    void (*code)(void);
#else
    int first, count;               /* in insns[] */
#endif
    int next_in_bucket;             /* -1 at the end */
} block_t;

static m68kjit_host_t H;
static block_t *blocks;
static int n_blocks;
static int bucket[1 << HASH_BITS];
m68kjit_stats_t m68kjit_stats;

#ifdef M68KJIT_XTENSA
static insn_t tmp[M68KJIT_MAX_INSNS];       /* the block being translated */
static xj_exec_t cache;
static size_t cache_used;
static uint8_t scratch[8192];
static uint32_t base;                       /* the lowest address of the state below */
static int o_ppc, o_pc, o_ir, o_dar, o_x, o_n, o_z, o_v, o_c;   /* offsets from base */
#else
static insn_t *insns;
static int n_insns;
#endif

bool m68kjit_init(const m68kjit_host_t *host)
{
    H = *host;
    if (!blocks) blocks = malloc(sizeof(block_t) * M68KJIT_MAX_BLOCKS);
    if (!blocks) return false;
#ifdef M68KJIT_XTENSA
    /* the state is reached with l32i/s32i from one base register: it must sit
     * within 1020 bytes (it is one struct in every Musashi) */
    uint32_t *const ptr[9] = {H.ppc, H.pc, H.ir, H.dar + 15, H.flag_x, H.flag_n, H.flag_z, H.flag_v, H.flag_c};
    int *const off[9] = {&o_ppc, &o_pc, &o_ir, &o_dar, &o_x, &o_n, &o_z, &o_v, &o_c};
    base = (uintptr_t)H.dar;
    for (int k = 0; k < 9; k++)
        if ((uintptr_t)ptr[k] < base) base = (uintptr_t)ptr[k];
    for (int k = 0; k < 9; k++)
    {
        *off[k] = (uintptr_t)ptr[k] - base;
        if (((uintptr_t)ptr[k] & 3) || *off[k] > 1020)
            return false;
    }
    o_dar = (uintptr_t)H.dar - base;
    if (!cache.data && !xj_exec_alloc_psram(&cache, M68KJIT_CODE_SIZE))
        return false;
#else
    if (!insns) insns = malloc(sizeof(insn_t) * M68KJIT_MAX_TOTAL);
    if (!insns) return false;
#endif
    m68kjit_flush();
    return true;
}

void m68kjit_flush(void)
{
    n_blocks = 0;
#ifdef M68KJIT_XTENSA
    cache_used = 0;
#else
    n_insns = 0;
#endif
    memset(bucket, 0xFF, sizeof(bucket));
}

/* instructions after which execution never falls through */
static bool ends_block(uint16_t op)
{
    switch (op >> 12)
    {
    case 0x6: return ((op >> 8) & 0xF) <= 1;                 /* BRA, BSR */
    case 0xA: case 0xF: return true;                         /* line A/F traps */
    case 0x4:
        if (op == 0x4AFC) return true;                       /* ILLEGAL */
        if ((op & 0xFFF0) == 0x4E40) return true;            /* TRAP */
        if ((op & 0xFF80) == 0x4E80) return true;            /* JSR, JMP */
        switch (op)
        {
        case 0x4E70: case 0x4E72: case 0x4E73: case 0x4E75: case 0x4E77:   /* RESET STOP RTE RTS RTR */
            return true;
        }
    }
    return false;
}

static int lookup(uint32_t pc)
{
    for (int b = bucket[HASH(pc)]; b >= 0; b = blocks[b].next_in_bucket)
        if (blocks[b].pc == pc)
            return b;
    return -1;
}

/* decode up to max instructions from pc into out[]; returns the count */
static int decode(uint32_t pc, insn_t *out, int max)
{
    int n = 0;
    while (n < max && H.is_code(pc, 2))
    {
        uint16_t op = H.read_code16(pc);
        int len = m68kjit_insn_len(op);
        if (!H.is_code(pc, len))
            break;
        insn_t *i = &out[n++];
        i->handler = H.handlers[op];
        i->pc = pc;
        i->next = pc + len;
        i->op = op;
        i->cyc = H.cyc[op];
        pc += len;
        if (ends_block(op))
            break;
    }
    return n;
}

#ifdef M68KJIT_XTENSA
/* ---- native instructions ----
 *
 * An instruction the translator knows becomes Xtensa code that updates
 * Musashi's registers and flags itself, with the flag values Musashi's
 * handlers produce (X/C in bit 8, N/V in bit 7, Z zero when set; the other
 * bits are don't-care, and differ between Musashi's 32- and 64-bit builds).
 * Scratch registers a8..a15 (no call in a native instruction). Everything
 * here touches registers only and cannot raise an exception, so it needs
 * neither PPC nor IR, and PC only when the block leaves after it.
 */
enum { SZ_B = 1, SZ_W = 2, SZ_L = 4 };
enum { N_NONE, N_MOVEQ, N_MOVE, N_MOVEA, N_ALU, N_ALUA, N_ADDQA, N_TST, N_CLR, N_SWAP, N_EXTW, N_EXTL, N_LEA, N_BCC, N_DBCC };
enum { A_ADD, A_SUB, A_CMP, A_AND, A_OR, A_EOR };

typedef struct
{
    int kind, size, alu;
    int dst;                /* register number 0..15 (A registers are 8..15) */
    int smode, sreg;        /* source: mode 0 (Dn), 1 (An) or 7 (#imm in simm) */
    uint32_t simm;
    int cc;                 /* Bcc/DBcc condition */
    uint32_t target;        /* Bcc/DBcc: where a taken branch goes */
} nat_t;

static inline int o_r(int r) { return o_dar + 4 * r; }

/* d = s >> sh, logical (srli takes 0..15 only) */
static void shr(xj_emit_t *e, int d, int s, int sh)
{
    if (sh < 16) xj_srli(e, d, s, sh);
    else xj_extui(e, d, s, sh, 32 - sh);
}

static uint32_t code32(uint32_t a) { return (uint32_t)H.read_code16(a) << 16 | H.read_code16(a + 2); }

/* source <ea> in register-only form: Dn, An (not for bytes) or #imm */
static bool reg_src(const insn_t *i, int size, bool an_ok, nat_t *n)
{
    int mode = (i->op >> 3) & 7, reg = i->op & 7;
    if (mode == 0) { n->smode = 0; n->sreg = reg; return true; }
    if (mode == 1 && an_ok && size != SZ_B) { n->smode = 1; n->sreg = 8 + reg; return true; }
    if (mode == 7 && reg == 4)
    {
        n->smode = 7;
        n->simm = size == SZ_L ? code32(i->pc + 2) : H.read_code16(i->pc + 2);
        if (size == SZ_B) n->simm &= 0xFF;
        return true;
    }
    return false;
}

static const int ss_size[4] = {SZ_B, SZ_W, SZ_L, 0};

static bool classify(const insn_t *i, nat_t *n)
{
    uint16_t op = i->op;
    int ss = (op >> 6) & 3, rx = (op >> 9) & 7, mode = (op >> 3) & 7, ry = op & 7;
    memset(n, 0, sizeof(*n));
    switch (op >> 12)
    {
    case 0x7:                                                   /* MOVEQ */
        if (op & 0x100) return false;
        n->kind = N_MOVEQ; n->dst = rx; n->simm = (uint32_t)(int8_t)op;
        return true;
    case 0x1: case 0x2: case 0x3:                               /* MOVE / MOVEA to a register */
    {
        int size = (op >> 12) == 1 ? SZ_B : (op >> 12) == 3 ? SZ_W : SZ_L;
        int dmode = (op >> 6) & 7;
        if (dmode == 0) n->kind = N_MOVE;
        else if (dmode == 1 && size != SZ_B) n->kind = N_MOVEA;
        else return false;
        n->size = size; n->dst = dmode == 1 ? 8 + rx : rx;
        return reg_src(i, size, true, n);
    }
    case 0x6:                                                   /* Bcc (not BRA/BSR, not the .l form) */
    {
        int cc = (op >> 8) & 0xF, d8 = op & 0xFF;
        if (cc <= 1 || d8 == 0xFF) return false;
        n->kind = N_BCC; n->cc = cc;
        n->target = i->pc + 2 + (d8 ? (int8_t)d8 : (int16_t)H.read_code16(i->pc + 2));
        return true;
    }
    case 0x5:                                                   /* ADDQ / SUBQ */
    {
        if (ss == 3 && mode == 1)                               /* DBcc */
        {
            n->kind = N_DBCC; n->cc = (op >> 8) & 0xF; n->dst = ry;
            n->target = i->pc + 2 + (int16_t)H.read_code16(i->pc + 2);
            return true;
        }
        if (ss == 3) return false;
        int q = rx ? rx : 8;
        bool sub = op & 0x100;
        if (mode == 0)
        {
            n->kind = N_ALU; n->alu = sub ? A_SUB : A_ADD; n->size = ss_size[ss]; n->dst = ry;
            n->smode = 7; n->simm = q;
            return true;
        }
        if (mode == 1 && ss != 0) { n->kind = N_ADDQA; n->dst = 8 + ry; n->simm = sub ? -q : q; return true; }
        return false;
    }
    case 0x9: case 0xD: case 0xB: case 0xC: case 0x8:           /* SUB ADD CMP/EOR AND OR */
    {
        int line = op >> 12;
        if (ss == 3)                                            /* SUBA ADDA CMPA (.w / .l) */
        {
            if (line == 0xC || line == 0x8) return false;       /* MULU/MULS, DIVU/DIVS */
            n->kind = N_ALUA; n->size = (op & 0x100) ? SZ_L : SZ_W; n->dst = 8 + rx;
            n->alu = line == 0x9 ? A_SUB : line == 0xD ? A_ADD : A_CMP;
            return reg_src(i, n->size, true, n);
        }
        n->size = ss_size[ss];
        if (op & 0x100)                                         /* Dn,<ea>: only EOR Dn,Dm here */
        {
            if (line != 0xB || mode != 0) return false;
            n->kind = N_ALU; n->alu = A_EOR; n->dst = ry; n->smode = 0; n->sreg = rx;
            return true;
        }
        n->kind = N_ALU; n->dst = rx;
        n->alu = line == 0x9 ? A_SUB : line == 0xD ? A_ADD : line == 0xB ? A_CMP : line == 0xC ? A_AND : A_OR;
        return reg_src(i, n->size, line != 0xC && line != 0x8, n);
    }
    case 0x4:
        if ((op & 0xFFC0) == 0x4A00 || (op & 0xFFC0) == 0x4A40 || (op & 0xFFC0) == 0x4A80)
        {                                                       /* TST Dn */
            if (mode != 0) return false;
            n->kind = N_TST; n->size = ss_size[ss]; n->dst = ry;
            return true;
        }
        if ((op & 0xFF00) == 0x4200 && ss != 3 && mode == 0)
        {                                                       /* CLR Dn */
            n->kind = N_CLR; n->size = ss_size[ss]; n->dst = ry;
            return true;
        }
        if ((op & 0xFFF8) == 0x4840) { n->kind = N_SWAP; n->dst = ry; return true; }
        if ((op & 0xFFF8) == 0x4880) { n->kind = N_EXTW; n->dst = ry; return true; }
        if ((op & 0xFFF8) == 0x48C0) { n->kind = N_EXTL; n->dst = ry; return true; }
        if ((op & 0xF1C0) == 0x41C0)                            /* LEA */
        {
            n->kind = N_LEA; n->dst = 8 + rx; n->smode = mode; n->sreg = 8 + ry;
            if (mode == 2 || mode == 5) return true;            /* (An), (d16,An) */
            if (mode == 7 && ry <= 2) { n->sreg = ry; return true; }   /* abs.w, abs.l, (d16,PC) */
            return false;
        }
        return false;
    }
    return false;
}

/* a8 = the source operand, masked to the size */
static void load_src(xj_block_t *b, const nat_t *n, int size)
{
    xj_emit_t *e = &b->e;
    if (n->smode == 7)
        xjb_imm(b, 8, n->simm);
    else
    {
        xj_l32i(e, 8, 2, o_r(n->sreg));
        if (size == SZ_B) xj_extui(e, 8, 8, 0, 8);
        else if (size == SZ_W) xj_extui(e, 8, 8, 0, 16);
    }
}

/* store the low size bytes of register r into Dn/An */
static void store_reg(xj_emit_t *e, int r, int reg, int size)
{
    if (size == SZ_B) xj_s8i(e, r, 2, o_r(reg));
    else if (size == SZ_W) xj_s16i(e, r, 2, o_r(reg));
    else xj_s32i(e, r, 2, o_r(reg));
}

/* N and Z from the result in register r (masked to the size), V = C = 0 */
static void flags_logic(xj_emit_t *e, int r, int size)
{
    if (size == SZ_B) xj_s32i(e, r, 2, o_n);
    else { shr(e, 9, r, size == SZ_W ? 8 : 24); xj_s32i(e, 9, 2, o_n); }
    xj_s32i(e, r, 2, o_z);
    xj_movi(e, 9, 0);
    xj_s32i(e, 9, 2, o_v);
    xj_s32i(e, 9, 2, o_c);
}

static void emit_native(xj_block_t *b, const insn_t *i, const nat_t *n)
{
    xj_emit_t *e = &b->e;
    switch (n->kind)
    {
    case N_MOVEQ:
        xjb_imm(b, 8, n->simm);
        xj_s32i(e, 8, 2, o_r(n->dst));
        flags_logic(e, 8, SZ_L);
        break;
    case N_MOVE:
        load_src(b, n, n->size);
        store_reg(e, 8, n->dst, n->size);
        flags_logic(e, 8, n->size);
        break;
    case N_MOVEA:
        load_src(b, n, SZ_L);
        if (n->size == SZ_W) xj_sext(e, 8, 8, 15);
        xj_s32i(e, 8, 2, o_r(n->dst));
        break;
    case N_ADDQA:
        xj_l32i(e, 9, 2, o_r(n->dst));
        xj_addi(e, 9, 9, (int32_t)n->simm);
        xj_s32i(e, 9, 2, o_r(n->dst));
        break;
    case N_ALUA:                                            /* ADDA SUBA CMPA: source sign-extended to 32 */
        load_src(b, n, SZ_L);
        if (n->size == SZ_W) xj_sext(e, 8, 8, 15);
        xj_l32i(e, 9, 2, o_r(n->dst));
        if (n->alu == A_ADD) { xj_add(e, 9, 9, 8); xj_s32i(e, 9, 2, o_r(n->dst)); break; }
        if (n->alu == A_SUB) { xj_sub(e, 9, 9, 8); xj_s32i(e, 9, 2, o_r(n->dst)); break; }
        /* CMPA: CMP.l flags */
        xj_sub(e, 10, 9, 8);
        xj_s32i(e, 10, 2, o_z);
        shr(e, 11, 10, 24);
        xj_s32i(e, 11, 2, o_n);
        xj_xor(e, 11, 8, 9);
        xj_xor(e, 12, 10, 9);
        xj_and(e, 11, 11, 12);
        shr(e, 11, 11, 24);
        xj_s32i(e, 11, 2, o_v);
        xj_saltu(e, 11, 9, 8);
        xj_slli(e, 11, 11, 8);
        xj_s32i(e, 11, 2, o_c);
        break;
    case N_ALU:
    {
        int size = n->size, sh = size == SZ_B ? 0 : size == SZ_W ? 8 : 24;
        load_src(b, n, size);                               /* a8 = src */
        xj_l32i(e, 9, 2, o_r(n->dst));                      /* a9 = dst */
        if (size != SZ_L) xj_extui(e, 9, 9, 0, size == SZ_B ? 8 : 16);
        switch (n->alu)
        {
        case A_AND: xj_and(e, 10, 9, 8); break;
        case A_OR:  xj_or(e, 10, 9, 8); break;
        case A_EOR: xj_xor(e, 10, 9, 8); break;
        case A_ADD: xj_add(e, 10, 9, 8); break;
        default:    xj_sub(e, 10, 9, 8); break;         /* SUB, CMP */
        }
        if (n->alu >= A_AND)
        {
            store_reg(e, 10, n->dst, size);
            flags_logic(e, 10, size);
            break;
        }
        /* N */
        if (sh) { shr(e, 11, 10, sh); xj_s32i(e, 11, 2, o_n); }
        else xj_s32i(e, 10, 2, o_n);
        /* V: add ((S^R) & (D^R)), sub/cmp ((S^D) & (R^D)) */
        if (n->alu == A_ADD) { xj_xor(e, 11, 8, 10); xj_xor(e, 12, 9, 10); }
        else { xj_xor(e, 11, 8, 9); xj_xor(e, 12, 10, 9); }
        xj_and(e, 11, 11, 12);
        if (sh) shr(e, 11, 11, sh);
        xj_s32i(e, 11, 2, o_v);
        /* C (and X): the carry/borrow in bit 8 */
        if (size == SZ_L)
        {
            if (n->alu == A_ADD) xj_saltu(e, 11, 10, 9);    /* res < dst */
            else xj_saltu(e, 11, 9, 8);                     /* dst < src */
            xj_slli(e, 11, 11, 8);
        }
        else if (size == SZ_W) xj_srli(e, 11, 10, 8);
        else xj_mov(e, 11, 10);
        xj_s32i(e, 11, 2, o_c);
        if (n->alu != A_CMP) xj_s32i(e, 11, 2, o_x);
        /* Z, and the result */
        if (size != SZ_L) xj_extui(e, 12, 10, 0, size == SZ_B ? 8 : 16);
        xj_s32i(e, size != SZ_L ? 12 : 10, 2, o_z);
        if (n->alu != A_CMP) store_reg(e, 10, n->dst, size);
        break;
    }
    case N_TST:
        xj_l32i(e, 8, 2, o_r(n->dst));
        if (n->size != SZ_L) xj_extui(e, 8, 8, 0, n->size == SZ_B ? 8 : 16);
        flags_logic(e, 8, n->size);
        break;
    case N_CLR:
        xj_movi(e, 8, 0);
        store_reg(e, 8, n->dst, n->size);
        flags_logic(e, 8, SZ_B);                            /* N = Z = V = C = 0 */
        break;
    case N_SWAP:
        xj_l32i(e, 8, 2, o_r(n->dst));
        xj_ssai(e, 16);
        xj_src(e, 8, 8, 8);
        xj_s32i(e, 8, 2, o_r(n->dst));
        flags_logic(e, 8, SZ_L);
        break;
    case N_EXTW:                                            /* low word = sign-extended low byte */
        xj_l32i(e, 8, 2, o_r(n->dst));
        xj_sext(e, 8, 8, 7);
        xj_s16i(e, 8, 2, o_r(n->dst));
        xj_extui(e, 8, 8, 0, 16);
        flags_logic(e, 8, SZ_W);
        break;
    case N_EXTL:
        xj_l32i(e, 8, 2, o_r(n->dst));
        xj_sext(e, 8, 8, 15);
        xj_s32i(e, 8, 2, o_r(n->dst));
        flags_logic(e, 8, SZ_L);
        break;
    case N_LEA:
        if (n->smode == 2) xj_l32i(e, 8, 2, o_r(n->sreg));
        else if (n->smode == 5)
        {
            xj_l32i(e, 8, 2, o_r(n->sreg));
            xjb_imm(b, 9, (uint32_t)(int16_t)H.read_code16(i->pc + 2));
            xj_add(e, 8, 8, 9);
        }
        else if (n->sreg == 0) xjb_imm(b, 8, (uint32_t)(int16_t)H.read_code16(i->pc + 2));
        else if (n->sreg == 1) xjb_imm(b, 8, code32(i->pc + 2));
        else xjb_imm(b, 8, i->pc + 2 + (int16_t)H.read_code16(i->pc + 2));
        xj_s32i(e, 8, 2, o_r(n->dst));
        break;
    }
}

/* branch to label when condition cc (2..15) holds, from Musashi's flags */
static void emit_cond(xj_block_t *b, int cc, int label)
{
    xj_emit_t *e = &b->e;
    int skip;
    switch (cc)
    {
    case 0x4: case 0x5:                                     /* CC CS: C bit 8 */
        xj_l32i(e, 8, 2, o_c);
        xjb_bbi(b, cc == 5 ? xj_bbsi : xj_bbci, 8, 8, label);
        break;
    case 0x6: case 0x7:                                     /* NE EQ: Z */
        xj_l32i(e, 8, 2, o_z);
        xjb_bz(b, cc == 6 ? xj_bnez : xj_beqz, 8, label);
        break;
    case 0x8: case 0x9:                                     /* VC VS: V bit 7 */
        xj_l32i(e, 8, 2, o_v);
        xjb_bbi(b, cc == 9 ? xj_bbsi : xj_bbci, 8, 7, label);
        break;
    case 0xA: case 0xB:                                     /* PL MI: N bit 7 */
        xj_l32i(e, 8, 2, o_n);
        xjb_bbi(b, cc == 0xB ? xj_bbsi : xj_bbci, 8, 7, label);
        break;
    case 0xC: case 0xD:                                     /* GE LT: (N ^ V) bit 7 */
        xj_l32i(e, 8, 2, o_n);
        xj_l32i(e, 9, 2, o_v);
        xj_xor(e, 8, 8, 9);
        xjb_bbi(b, cc == 0xD ? xj_bbsi : xj_bbci, 8, 7, label);
        break;
    case 0x2:                                               /* HI: C clear and Z not set */
        skip = xjb_label(b);
        xj_l32i(e, 8, 2, o_c);
        xjb_bbi(b, xj_bbsi, 8, 8, skip);
        xj_l32i(e, 8, 2, o_z);
        xjb_bz(b, xj_bnez, 8, label);
        xjb_bind(b, skip);
        break;
    case 0x3:                                               /* LS: C set or Z set */
        xj_l32i(e, 8, 2, o_c);
        xjb_bbi(b, xj_bbsi, 8, 8, label);
        xj_l32i(e, 8, 2, o_z);
        xjb_bz(b, xj_beqz, 8, label);
        break;
    case 0xE:                                               /* GT: GE and NE */
        skip = xjb_label(b);
        xj_l32i(e, 8, 2, o_n);
        xj_l32i(e, 9, 2, o_v);
        xj_xor(e, 8, 8, 9);
        xjb_bbi(b, xj_bbsi, 8, 7, skip);
        xj_l32i(e, 8, 2, o_z);
        xjb_bz(b, xj_bnez, 8, label);
        xjb_bind(b, skip);
        break;
    case 0xF:                                               /* LE: LT or EQ */
        xj_l32i(e, 8, 2, o_n);
        xj_l32i(e, 9, 2, o_v);
        xj_xor(e, 8, 8, 9);
        xjb_bbi(b, xj_bbsi, 8, 7, label);
        xj_l32i(e, 8, 2, o_z);
        xjb_bz(b, xj_beqz, 8, label);
        break;
    }
}

/* cycles -= c (a6 = the new count) */
static void emit_cycles(xj_emit_t *e, int c)
{
    xj_l32i(e, 6, 3, 0);
    if (c <= 128)
        xj_addi(e, 6, 6, -c);
    else
    {
        xj_movi(e, 7, c);
        xj_sub(e, 6, 6, 7);
    }
    xj_s32i(e, 6, 3, 0);
}

/* The block, as Xtensa (windowed ABI, called with callx8 from m68kjit_run):
 *
 *     entry a1, 32
 *     a2 = base (Musashi's state), a3 = &cycles, a5 = pc of the instruction
 *   an instruction run by its handler:
 *     s32i a5, a2, PPC;  addi a7, a5, 2;  s32i a7, a2, PC
 *     a6 = op;  s32i a6, a2, IR
 *     a8 = handler;  callx8 a8               (a2..a7 survive the call)
 *     l32i a6, a3, 0;  a6 -= cyc;  s32i a6, a3, 0
 *     l32i a7, a2, PC;  addi a5, a5, len
 *     beq a7, a5, 1f;  retw.n;  1:           leave on a PC change
 *     bgei a6, 1, 2f;  retw.n;  2:           leave when the slice is over
 *   a native instruction:
 *     <its code>;  cycles as above
 *     bgei a6, 1, 2f;  addi a7, a5, len;  s32i a7, a2, PC;  retw.n;  2:
 *     addi a5, a5, len
 *   retw.n                                   (after storing PC if the last was native)
 */
static void emit_block(xj_block_t *b, const insn_t *in, int n)
{
    xj_emit_t *e = &b->e;
    xj_entry(e, 1, 32);
    xjb_lit(b, 2, base);
    xjb_lit(b, 3, (uint32_t)(uintptr_t)H.cycles);
    xjb_imm(b, 5, in[0].pc);
    for (int k = 0; k < n; k++)
    {
        const insn_t *i = &in[k];
        int len = i->next - i->pc;
        bool last = k == n - 1;
        nat_t nat;
        if (classify(i, &nat) && (nat.kind == N_BCC || nat.kind == N_DBCC))
        {
            /* taken: cycles, PC = target, leave. Not taken: cycles, go on (or
             * leave at the end of the slice / of the block with PC = next) */
            m68kjit_stats.native++;
            int taken = xjb_label(b), fall = xjb_label(b);
            int cyc_fall = i->cyc;
            if (nat.kind == N_BCC)
            {
                emit_cond(b, nat.cc, taken);
                cyc_fall += len == 2 ? H.cyc_bcc_notake_b : H.cyc_bcc_notake_w;
            }
            else if (nat.cc != 0)                           /* DBcc: the condition holds -> fall through */
            {
                int dec = xjb_label(b);
                if (nat.cc == 1) xjb_j(b, dec);             /* DBF: never holds */
                else { emit_cond(b, nat.cc, fall); xjb_j(b, dec); }
                xjb_bind(b, dec);
                /* Dn.w -= 1; taken unless it became 0xFFFF */
                xj_l32i(e, 8, 2, o_r(nat.dst));
                xj_addi(e, 8, 8, -1);
                xj_s16i(e, 8, 2, o_r(nat.dst));
                xj_extui(e, 8, 8, 0, 16);
                xjb_imm(b, 9, 0xFFFF);
                int expired = xjb_label(b);
                xjb_b8(b, xj_beq, 8, 9, expired);
                emit_cycles(e, i->cyc + H.cyc_dbcc_f_noexp);
                xjb_imm(b, 7, nat.target);
                xj_s32i(e, 7, 2, o_pc);
                xj_retw_n(e);
                xjb_bind(b, expired);
                emit_cycles(e, i->cyc + H.cyc_dbcc_f_exp);
                int go = xjb_label(b);
                xjb_j(b, go);
                xjb_bind(b, fall);
                emit_cycles(e, i->cyc);
                xjb_bind(b, go);
                cyc_fall = -1;                              /* cycles already done on both paths */
            }
            if (nat.kind == N_BCC)
            {
                /* not taken */
                emit_cycles(e, cyc_fall);
                int go = xjb_label(b);
                xjb_j(b, go);
                xjb_bind(b, taken);
                emit_cycles(e, i->cyc);
                xjb_imm(b, 7, nat.target);
                xj_s32i(e, 7, 2, o_pc);
                xj_retw_n(e);
                xjb_bind(b, go);
            }
            else if (nat.cc == 0)                           /* DBT: plain fall through */
                emit_cycles(e, cyc_fall);
            if (last)
            {
                xj_addi(e, 7, 5, len);
                xj_s32i(e, 7, 2, o_pc);
                break;
            }
            int more = xjb_label(b);
            xjb_bi(b, xj_bgei, 6, 1, more);
            xj_addi(e, 7, 5, len);
            xj_s32i(e, 7, 2, o_pc);
            xj_retw_n(e);
            xjb_bind(b, more);
            xj_addi(e, 5, 5, len);
            continue;
        }
        if (classify(i, &nat))
        {
            m68kjit_stats.native++;
            emit_native(b, i, &nat);
            emit_cycles(e, i->cyc);
            if (last)
            {
                xj_addi(e, 7, 5, len);
                xj_s32i(e, 7, 2, o_pc);
                break;
            }
            int more = xjb_label(b);
            xjb_bi(b, xj_bgei, 6, 1, more);
            xj_addi(e, 7, 5, len);
            xj_s32i(e, 7, 2, o_pc);
            xj_retw_n(e);
            xjb_bind(b, more);
            xj_addi(e, 5, 5, len);
            continue;
        }
        xj_s32i(e, 5, 2, o_ppc);
        xj_addi(e, 7, 5, 2);
        xj_s32i(e, 7, 2, o_pc);
        xjb_imm(b, 6, i->op);
        xj_s32i(e, 6, 2, o_ir);
        xjb_lit(b, 8, (uint32_t)(uintptr_t)i->handler);
        xj_callx8(e, 8);
        emit_cycles(e, i->cyc);
        if (last)
            break;                          /* the last one leaves anyway */
        xj_l32i(e, 7, 2, o_pc);
        xj_addi(e, 5, 5, len);
        int same = xjb_label(b);
        xjb_b8(b, xj_beq, 7, 5, same);
        xj_retw_n(e);
        xjb_bind(b, same);
        int more = xjb_label(b);
        xjb_bi(b, xj_bgei, 6, 1, more);
        xj_retw_n(e);
        xjb_bind(b, more);
    }
    xj_retw_n(e);
}

static void *install(xj_block_t *b)
{
    size_t off = (cache_used + 3) & ~(size_t)3;
    int entry;
    int size = xjb_finalize(b, cache.data + off, cache.exec + off, (int)(cache.size - off), xj_exec_write_word, &entry);
    if (size < 0 || xj_exec_sync(&cache, off, size))
        return NULL;
    cache_used = off + size;
    return xj_exec_ptr(&cache, off + entry);
}
#endif

static int translate(uint32_t pc)
{
#ifdef M68KJIT_XTENSA
    if (n_blocks == M68KJIT_MAX_BLOCKS || cache_used + sizeof(scratch) > cache.size)
        m68kjit_flush();
    int n = decode(pc, tmp, M68KJIT_MAX_INSNS);
    void *code = NULL;
    while (n > 0)
    {
        static xj_block_t b;                /* ~12 KB: never on the caller's stack */
        xjb_init(&b, scratch, sizeof(scratch));
        emit_block(&b, tmp, n);
        if (xjb_ok(&b) && (code = install(&b)))
            break;
        n /= 2;                             /* too many literals or too big: shorter */
    }
    if (!code)
        return -1;
    block_t *bl = &blocks[n_blocks];
    bl->code = (void (*)(void))code;
#else
    if (n_blocks == M68KJIT_MAX_BLOCKS || n_insns + M68KJIT_MAX_INSNS > M68KJIT_MAX_TOTAL)
        m68kjit_flush();
    int n = decode(pc, &insns[n_insns], M68KJIT_MAX_INSNS);
    if (n == 0)
        return -1;
    block_t *bl = &blocks[n_blocks];
    bl->first = n_insns;
    bl->count = n;
    n_insns += n;
#endif
    bl->pc = pc;
    int h = HASH(pc);
    bl->next_in_bucket = bucket[h];
    bucket[h] = n_blocks;
    m68kjit_stats.blocks++;
    m68kjit_stats.insns += n;
    return n_blocks++;
}

static void run_block(const block_t *b)
{
    m68kjit_stats.block_runs++;
#ifdef M68KJIT_XTENSA
    b->code();
#else
    const insn_t *i = &insns[b->first], *end = i + b->count;
    for (; i < end; i++)
    {
        m68kjit_stats.block_insns++;
        *H.ppc = i->pc;
        *H.pc = i->pc + 2;
        *H.ir = i->op;
        i->handler();
        *H.cycles -= i->cyc;
        if (*H.pc != i->next) { m68kjit_stats.exits_pc++; return; }
        if (*H.cycles <= 0) { m68kjit_stats.exits_cycles++; return; }
    }
#endif
}

void m68kjit_run(void)
{
    do
    {
        uint32_t pc = *H.pc;
        int b = lookup(pc);
        if (b < 0 && H.is_code(pc, 2))
            b = translate(pc);
#ifdef M68KJIT_TRACE
        if (b >= 0 && m68kjit_stats.block_runs < 30)
            printf("TRACE pc %06X blk %d n_blocks %d cyc %d bucket0 %d\n", (unsigned)pc, b, n_blocks, (int)*H.cycles, bucket[0]);
#endif
        if (b >= 0)
            run_block(&blocks[b]);
        else
        {
            m68kjit_stats.steps++;
            H.step();
        }
    } while (*H.cycles > 0);
}
