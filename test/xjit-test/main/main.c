/* xjit-test: the shared Xtensa JIT core (components/xjit) on the ESP32-S3.
 *
 * Step 0: code written at run time runs from internal RAM and from PSRAM.
 * Step 2: blocks built with xjit_block.h (labels, literal pool, calls into C,
 *         loads/stores, every branch kind) give the expected results, and a
 *         differential test runs random ALU programs both as generated code
 *         and through a C reference interpreter.
 * Every test runs from both code caches. Results are "XJIT ..." lines, the
 * last one "XJIT RESULT <pass>/<total>". Cycle counts only mean something
 * on the board (QEMU models no caches or PSRAM timing). */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_chip_info.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xjit_block.h"
#include "xjit_exec.h"
#include "thumb.h"

static int tests, passed;
static void check(const char *name, const char *cache, int ok, const char *detail)
{
    tests++;
    passed += ok != 0;
    if (!ok || detail)
        printf("XJIT %-6s %-28s %s %s\n", cache, name, ok ? "PASS" : "FAIL", detail ? detail : "");
}

/* ---- code caches ----------------------------------------------------------- */
static xj_exec_t caches[2];
static const char *cache_name[2] = {"IRAM", "PSRAM"};
static size_t cache_used[2];
static uint8_t scratch[16384];

/* lay a finished block out in cache c; returns its entry point or NULL */
static void *install(xj_block_t *b, int c)
{
    xj_exec_t *x = &caches[c];
    size_t off = (cache_used[c] + 15) & ~(size_t)15;
    int entry;
    int n = xjb_finalize(b, x->data + off, x->exec + off, (int)(x->size - off), xj_exec_write_word, &entry);
    if (n < 0)
        return NULL;
    int err = xj_exec_sync(x, off, n);
    if (err)
    {
        static int reported;
        if (reported++ < 3)
            printf("XJIT %s cache sync error %d at offset %u\n", cache_name[c], err, (unsigned)off);
        return NULL;
    }
    cache_used[c] = off + n;
    return xj_exec_ptr(x, off + entry);
}

/* ---- step 2: hand-built blocks --------------------------------------------- */
typedef int (*fn1_t)(int);
typedef int (*fn2_t)(int, int);
typedef uint32_t (*fnp_t)(void *);

static int c_helper(int x, int y) { return x * 3 + y; }

/* add(a, b) */
static void gen_add(xj_block_t *b)
{
    xj_entry(&b->e, 1, 32);
    xj_add(&b->e, 2, 2, 3);
    xj_retw_n(&b->e);
}
/* sum(n) = n + (n-1) + ... + 1, with a forward and a backward label */
static void gen_sum(xj_block_t *b)
{
    int loop = xjb_label(b), done = xjb_label(b);
    xj_entry(&b->e, 1, 32);
    xj_movi(&b->e, 4, 0);
    xjb_bz(b, xj_beqz, 2, done);
    xjb_bind(b, loop);
    xj_add(&b->e, 4, 4, 2);
    xj_addi(&b->e, 2, 2, -1);
    xjb_bz(b, xj_bnez, 2, loop);
    xjb_bind(b, done);
    xj_mov_n(&b->e, 2, 4);
    xj_retw_n(&b->e);
}
/* consts(sel): 0x12345678, 0xDEADBEEF, -5 (movi), 0x12345678 again (pooled once) */
static void gen_consts(xj_block_t *b)
{
    int l1 = xjb_label(b), l2 = xjb_label(b), l3 = xjb_label(b);
    xj_entry(&b->e, 1, 32);
    xjb_bi(b, xj_beqi, 2, 1, l1);
    xjb_bi(b, xj_beqi, 2, 2, l2);
    xjb_bi(b, xj_beqi, 2, 3, l3);
    xjb_imm(b, 2, 0x12345678);
    xj_retw_n(&b->e);
    xjb_bind(b, l1);
    xjb_imm(b, 2, 0xDEADBEEF);
    xj_retw_n(&b->e);
    xjb_bind(b, l2);
    xjb_imm(b, 2, (uint32_t)-5);
    xj_retw_n(&b->e);
    xjb_bind(b, l3);
    xjb_lit(b, 2, 0x12345678);
    xj_retw_n(&b->e);
}
/* mem(p): p[2] = *(u32*)p + *(s16*)(p+4) + *(u8*)(p+6); returns p[2] and writes
   the low byte to p+12 and the low half to p+14 */
static void gen_mem(xj_block_t *b)
{
    xj_entry(&b->e, 1, 32);
    xj_l32i(&b->e, 3, 2, 0);
    xj_l16si(&b->e, 4, 2, 4);
    xj_l8ui(&b->e, 5, 2, 6);
    xj_add(&b->e, 3, 3, 4);
    xj_add(&b->e, 3, 3, 5);
    xj_s32i(&b->e, 3, 2, 8);
    xj_s8i(&b->e, 3, 2, 12);
    xj_s16i(&b->e, 3, 2, 14);
    xj_memw(&b->e);
    xj_mov_n(&b->e, 2, 3);
    xj_retw_n(&b->e);
}
/* call(x) = c_helper(x, 7) + 1, through callx8 (args a10.., result a10) */
static void gen_call(xj_block_t *b)
{
    xj_entry(&b->e, 1, 32);
    xj_mov_n(&b->e, 10, 2);
    xj_movi_n(&b->e, 11, 7);
    xjb_lit(b, 8, (uint32_t)(uintptr_t)c_helper);
    xj_callx8(&b->e, 8);
    xj_addi(&b->e, 2, 10, 1);
    xj_retw_n(&b->e);
}
/* classify(x): a bit per branch kind that is taken for x */
static void gen_branches(xj_block_t *b)
{
    struct { int kind; void (*f3)(xj_emit_t *, int, int, int); void (*f2)(xj_emit_t *, int, int); int imm; } br[] = {
        {0, xj_blt, NULL, 0}, {0, xj_bltu, NULL, 0}, {0, xj_bge, NULL, 0}, {0, xj_bgeu, NULL, 0},
        {0, xj_beq, NULL, 0}, {0, xj_bne, NULL, 0}, {0, xj_bany, NULL, 0}, {0, xj_bnone, NULL, 0},
        {0, xj_ball, NULL, 0}, {0, xj_bnall, NULL, 0}, {0, xj_bbc, NULL, 0}, {0, xj_bbs, NULL, 0},
        {1, NULL, xj_beqz, 0}, {1, NULL, xj_bnez, 0}, {1, NULL, xj_bltz, 0}, {1, NULL, xj_bgez, 0},
        {2, xj_beqi, NULL, 10}, {2, xj_bnei, NULL, 10}, {2, xj_blti, NULL, 10}, {2, xj_bgei, NULL, 10},
        {3, xj_bbci, NULL, 3}, {3, xj_bbsi, NULL, 3}, {4, NULL, NULL, 32768}, {5, NULL, NULL, 32768},
        {6, NULL, NULL, 0}, {6, NULL, NULL, 1},
    };
    xj_entry(&b->e, 1, 32);
    xj_movi(&b->e, 3, 100);          /* a3 = 100: the other operand of the two-register branches */
    xj_movi(&b->e, 4, 0);            /* a4 = result bits */
    for (int i = 0; i < (int)(sizeof(br) / sizeof(br[0])); i++)
    {
        int skip = xjb_label(b);
        switch (br[i].kind)
        {
        case 0: xjb_b8(b, br[i].f3, 2, 3, skip); break;
        case 1: xjb_bz(b, br[i].f2, 2, skip); break;
        case 2: xjb_bi(b, br[i].f3, 2, br[i].imm, skip); break;
        case 3: xjb_bbi(b, br[i].f3, 2, br[i].imm, skip); break;
        case 4: xjb_bi(b, (void (*)(xj_emit_t *, int, int, int))xj_bltui, 2, br[i].imm, skip); break;
        case 5: xjb_bi(b, (void (*)(xj_emit_t *, int, int, int))xj_bgeui, 2, br[i].imm, skip); break;
        case 6: xjb_bz_n(b, br[i].imm, 2, skip); break;
        }
        /* not taken: set bit i */
        xjb_imm(b, 5, 1u << i);
        xj_or(&b->e, 4, 4, 5);
        xjb_bind(b, skip);
    }
    xj_mov_n(&b->e, 2, 4);
    xj_retw_n(&b->e);
}
static uint32_t ref_branches(int32_t x)
{
    uint32_t u = (uint32_t)x, r = 0;
    int taken[26] = {
        x < 100, u < 100u, x >= 100, u >= 100u, x == 100, x != 100, (u & 100) != 0, (u & 100) == 0,
        (u & 100) == 100, (u & 100) != 100, !(u >> (100 & 31) & 1), (u >> (100 & 31) & 1),
        x == 0, x != 0, x < 0, x >= 0, x == 10, x != 10, x < 10, x >= 10,
        !(u >> 3 & 1), (u >> 3 & 1), u < 32768u, u >= 32768u, x == 0, x != 0,
    };
    for (int i = 0; i < 26; i++)
        if (!taken[i])
            r |= 1u << i;
    return r;
}

/* ---- step 2: differential test of the ALU ----------------------------------- */
/* A random program of ops on a2..a7 (a2..a5 = the four arguments); the result
   is the xor of a2..a7. The same program runs in C. */
enum { OP_ADD, OP_SUB, OP_AND, OP_OR, OP_XOR, OP_ADDX2, OP_ADDX4, OP_ADDX8, OP_SUBX2, OP_SUBX4, OP_SUBX8,
       OP_MULL, OP_MULUH, OP_MULSH, OP_MIN, OP_MAX, OP_MINU, OP_MAXU, OP_MOVEQZ, OP_MOVNEZ, OP_MOVLTZ, OP_MOVGEZ,
       OP_NEG, OP_ABS, OP_SEXT, OP_NSAU, OP_SLLI, OP_SRLI, OP_SRAI, OP_EXTUI, OP_SLL, OP_SRL, OP_SRA, OP_SRC,
       OP_ADDI, OP_ADDMI, OP_MOVI, OP_IMM32, OP_COUNT };
typedef struct { uint8_t op, r, s, t; int32_t k, k2; } insn_t;
#define PROG_LEN 24

static uint32_t rnd_state = 12345;
static uint32_t rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 17; rnd_state ^= rnd_state << 5; return rnd_state; }
static int rreg(void) { return 2 + rnd() % 6; }

static void random_prog(insn_t *p)
{
    for (int i = 0; i < PROG_LEN; i++)
    {
        insn_t *q = &p[i];
        q->op = rnd() % OP_COUNT; q->r = rreg(); q->s = rreg(); q->t = rreg();
        switch (q->op)
        {
        case OP_SEXT: q->k = 7 + rnd() % 16; break;
        case OP_SLLI: q->k = 1 + rnd() % 31; break;
        case OP_SRLI: q->k = rnd() % 16; break;
        case OP_SRAI: q->k = rnd() % 32; break;
        case OP_EXTUI: q->k = rnd() % 32; q->k2 = 1 + rnd() % (q->k > 16 ? 32 - q->k : 16); break;
        case OP_SLL: case OP_SRL: case OP_SRA: case OP_SRC: q->k = rreg(); break;   /* the shift amount register */
        case OP_ADDI: q->k = (int8_t)rnd(); break;
        case OP_ADDMI: q->k = (int8_t)rnd() * 256; break;
        case OP_MOVI: q->k = (int)(rnd() % 4096) - 2048; break;
        case OP_IMM32: q->k = (int32_t)rnd(); break;
        }
    }
}

static uint32_t ref_prog(const insn_t *p, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t R[16] = {0};
    R[2] = a; R[3] = b; R[4] = c; R[5] = d; R[6] = 0x5A5A5A5A; R[7] = 0x01234567;
    for (int i = 0; i < PROG_LEN; i++)
    {
        const insn_t *q = &p[i];
        uint32_t s = R[q->s], t = R[q->t], v = R[q->r];
        int32_t ss = (int32_t)s, ts = (int32_t)t;
        uint32_t sa;
        switch (q->op)
        {
        case OP_ADD: v = s + t; break;
        case OP_SUB: v = s - t; break;
        case OP_AND: v = s & t; break;
        case OP_OR: v = s | t; break;
        case OP_XOR: v = s ^ t; break;
        case OP_ADDX2: v = (s << 1) + t; break;
        case OP_ADDX4: v = (s << 2) + t; break;
        case OP_ADDX8: v = (s << 3) + t; break;
        case OP_SUBX2: v = (s << 1) - t; break;
        case OP_SUBX4: v = (s << 2) - t; break;
        case OP_SUBX8: v = (s << 3) - t; break;
        case OP_MULL: v = s * t; break;
        case OP_MULUH: v = (uint32_t)(((uint64_t)s * t) >> 32); break;
        case OP_MULSH: v = (uint32_t)(((int64_t)ss * ts) >> 32); break;
        case OP_MIN: v = ss < ts ? s : t; break;
        case OP_MAX: v = ss > ts ? s : t; break;
        case OP_MINU: v = s < t ? s : t; break;
        case OP_MAXU: v = s > t ? s : t; break;
        case OP_MOVEQZ: if (t == 0) v = s; break;
        case OP_MOVNEZ: if (t != 0) v = s; break;
        case OP_MOVLTZ: if (ts < 0) v = s; break;
        case OP_MOVGEZ: if (ts >= 0) v = s; break;
        case OP_NEG: v = -t; break;
        case OP_ABS: v = ts < 0 ? -t : t; break;
        case OP_SEXT: v = (uint32_t)((int32_t)(s << (31 - q->k)) >> (31 - q->k)); break;
        case OP_NSAU: v = s ? __builtin_clz(s) : 32; break;
        case OP_SLLI: v = s << q->k; break;
        case OP_SRLI: v = t >> q->k; break;
        case OP_SRAI: v = (uint32_t)(ts >> q->k); break;
        case OP_EXTUI: v = (t >> q->k) & (q->k2 == 32 ? ~0u : ((1u << q->k2) - 1)); break;
        case OP_SLL: sa = R[q->k] & 31; v = sa ? s << sa : s; break;   /* ssl: SAR = 32 - sa */
        case OP_SRL: sa = R[q->k] & 31; v = t >> sa; break;
        case OP_SRA: sa = R[q->k] & 31; v = (uint32_t)(ts >> sa); break;
        case OP_SRC: sa = R[q->k] & 31; v = (uint32_t)((((uint64_t)s << 32) | t) >> sa); break;
        case OP_ADDI: case OP_ADDMI: v = s + q->k; break;
        case OP_MOVI: case OP_IMM32: v = (uint32_t)q->k; break;
        }
        R[q->r] = v;
    }
    return R[2] ^ R[3] ^ R[4] ^ R[5] ^ R[6] ^ R[7];
}

static void gen_prog(xj_block_t *b, const insn_t *p)
{
    xj_emit_t *e = &b->e;
    xj_entry(e, 1, 32);
    xjb_imm(b, 6, 0x5A5A5A5A);
    xjb_imm(b, 7, 0x01234567);
    for (int i = 0; i < PROG_LEN; i++)
    {
        const insn_t *q = &p[i];
        int r = q->r, s = q->s, t = q->t;
        switch (q->op)
        {
        case OP_ADD: xj_add(e, r, s, t); break;
        case OP_SUB: xj_sub(e, r, s, t); break;
        case OP_AND: xj_and(e, r, s, t); break;
        case OP_OR: xj_or(e, r, s, t); break;
        case OP_XOR: xj_xor(e, r, s, t); break;
        case OP_ADDX2: xj_addx2(e, r, s, t); break;
        case OP_ADDX4: xj_addx4(e, r, s, t); break;
        case OP_ADDX8: xj_addx8(e, r, s, t); break;
        case OP_SUBX2: xj_subx2(e, r, s, t); break;
        case OP_SUBX4: xj_subx4(e, r, s, t); break;
        case OP_SUBX8: xj_subx8(e, r, s, t); break;
        case OP_MULL: xj_mull(e, r, s, t); break;
        case OP_MULUH: xj_muluh(e, r, s, t); break;
        case OP_MULSH: xj_mulsh(e, r, s, t); break;
        case OP_MIN: xj_min(e, r, s, t); break;
        case OP_MAX: xj_max(e, r, s, t); break;
        case OP_MINU: xj_minu(e, r, s, t); break;
        case OP_MAXU: xj_maxu(e, r, s, t); break;
        case OP_MOVEQZ: xj_moveqz(e, r, s, t); break;
        case OP_MOVNEZ: xj_movnez(e, r, s, t); break;
        case OP_MOVLTZ: xj_movltz(e, r, s, t); break;
        case OP_MOVGEZ: xj_movgez(e, r, s, t); break;
        case OP_NEG: xj_neg(e, r, t); break;
        case OP_ABS: xj_abs(e, r, t); break;
        case OP_SEXT: xj_sext(e, r, s, q->k); break;
        case OP_NSAU: xj_nsau(e, r, s); break;
        case OP_SLLI: xj_slli(e, r, s, q->k); break;
        case OP_SRLI: xj_srli(e, r, t, q->k); break;
        case OP_SRAI: xj_srai(e, r, t, q->k); break;
        case OP_EXTUI: xj_extui(e, r, t, q->k, q->k2); break;
        case OP_SLL: xj_ssl(e, q->k); xj_sll(e, r, s); break;
        case OP_SRL: xj_ssr(e, q->k); xj_srl(e, r, t); break;
        case OP_SRA: xj_ssr(e, q->k); xj_sra(e, r, t); break;
        case OP_SRC: xj_ssr(e, q->k); xj_src(e, r, s, t); break;
        case OP_ADDI: xj_addi(e, r, s, q->k); break;
        case OP_ADDMI: xj_addmi(e, r, s, q->k); break;
        case OP_MOVI: xj_movi(e, r, q->k); break;
        case OP_IMM32: xjb_imm(b, r, (uint32_t)q->k); break;
        }
    }
    xj_xor(e, 2, 2, 3);
    xj_xor(e, 2, 2, 4);
    xj_xor(e, 2, 2, 5);
    xj_xor(e, 2, 2, 6);
    xj_xor(e, 2, 2, 7);
    xj_retw_n(e);
}
typedef uint32_t (*fn4_t)(uint32_t, uint32_t, uint32_t, uint32_t);

static void *build(void (*gen)(xj_block_t *), int c)
{
    static xj_block_t b;
    xjb_init(&b, scratch, sizeof(scratch));
    gen(&b);
    return install(&b, c);
}

static void run_blocks(int c)
{
    char d[96];
    const char *cn = cache_name[c];

    fn2_t add = build(gen_add, c);
    check("add", cn, add && add(40, 2) == 42 && add(-5, 3) == -2, NULL);

    fn1_t sum = build(gen_sum, c);
    check("sum loop (labels)", cn, sum && sum(0) == 0 && sum(1) == 1 && sum(100) == 5050, NULL);

    fn1_t k = build(gen_consts, c);
    check("constants (pool, movi)", cn, k && k(0) == 0x12345678 && (uint32_t)k(1) == 0xDEADBEEF && k(2) == -5 && k(3) == 0x12345678, NULL);

    fnp_t mem = build(gen_mem, c);
    {
        uint8_t m[16] __attribute__((aligned(4))) = {0x78, 0x56, 0x34, 0x12, 0xFE, 0xFF, 0x80, 0};
        uint32_t r = mem ? mem(m) : 0, want = 0x12345678u - 2 + 0x80, st;
        memcpy(&st, m + 8, 4);
        check("loads/stores", cn, mem && r == want && st == want && m[12] == (want & 0xFF) && m[14] == (want & 0xFF) && m[15] == ((want >> 8) & 0xFF), NULL);
    }

    fn1_t call = build(gen_call, c);
    check("callx8 into C", cn, call && call(5) == 5 * 3 + 7 + 1, NULL);

    fn1_t cls = build(gen_branches, c);
    {
        static const int32_t xs[] = {0, 1, 3, 8, 10, 11, 99, 100, 101, 36, 1000, 32767, 32768, -1, -100, 0x7FFFFFFF, (int32_t)0x80000000, 108, 4};
        int ok = cls != NULL, bad = -1;
        for (int i = 0; ok && i < (int)(sizeof(xs) / sizeof(xs[0])); i++)
            if ((uint32_t)cls(xs[i]) != ref_branches(xs[i])) { ok = 0; bad = i; }
        if (!ok && cls)
            snprintf(d, sizeof(d), "x=%" PRId32 " got %08" PRIx32 " want %08" PRIx32, xs[bad], (uint32_t)cls(xs[bad]), ref_branches(xs[bad]));
        check("26 branch kinds", cn, ok, ok ? NULL : d);
    }

    /* differential */
    {
        static insn_t prog[PROG_LEN];
        static xj_block_t b;
        int progs = 0, fails = 0;
        size_t mark = cache_used[c];
        for (int n = 0; n < 2000; n++)
        {
            random_prog(prog);
            xjb_init(&b, scratch, sizeof(scratch));
            gen_prog(&b, prog);
            fn4_t f = install(&b, c);
            if (!f) { fails++; continue; }
            progs++;
            for (int v = 0; v < 8; v++)
            {
                uint32_t a = rnd(), bb = rnd(), cc = v & 1 ? 0 : rnd(), dd = v & 2 ? (uint32_t)-(int32_t)(rnd() & 255) : rnd();
                uint32_t got = f(a, bb, cc, dd), want = ref_prog(prog, a, bb, cc, dd);
                if (got != want)
                {
                    if (fails++ < 3)
                        printf("XJIT diff prog %d op0 %d: got %08" PRIx32 " want %08" PRIx32 "\n", n, prog[0].op, got, want);
                    break;
                }
            }
            if (cache_used[c] > caches[c].size - 4096)
                cache_used[c] = mark;   /* reuse the cache: the old blocks are done */
        }
        snprintf(d, sizeof(d), "%d programs x 8 inputs, %d failures", progs, fails);
        check("differential ALU", cn, fails == 0 && progs == 2000, d);
    }
}



/* ---- step 4: Thumb translator against its reference ------------------------ */
static void run_thumb(int c)
{
    static uint16_t code[32];
    static xj_block_t b;
    static char d[160];
    int seqs = 0, fails = 0, untranslated = 0;
    size_t mark = cache_used[c];
    for (int n = 0; n < 3000; n++)
    {
        int count = 1 + (int)(rnd() % 24);
        for (int i = 0; i < count; i++)
            code[i] = thumb_random_op();
        xjb_init(&b, scratch, sizeof(scratch));
        if (!thumb_translate(&b, code, count)) { untranslated++; continue; }
        void (*f)(thumb_state_t *) = install(&b, c);
        if (!f) { untranslated++; continue; }
        seqs++;
        for (int v = 0; v < 6; v++)
        {
            thumb_state_t s0, want, got;
            thumb_random_state(&s0);
            want = got = s0;
            thumb_ref(&want, code, count);
            f(&got);
            if (memcmp(&want, &got, sizeof(want)))
            {
                if (fails++ < 4)
                {
                    int k = 0;
                    while (k < 20 && ((uint32_t *)&want)[k] == ((uint32_t *)&got)[k]) k++;
                    printf("XJIT thumb diff seq %d (%d ops, first %04x): word %d got %08" PRIx32 " want %08" PRIx32 "\n",
                           n, count, code[0], k, ((uint32_t *)&got)[k], ((uint32_t *)&want)[k]);
                    if (count <= 4)
                        for (int i = 0; i < count; i++) printf("XJIT   op %04x\n", code[i]);
                }
                break;
            }
        }
        if (cache_used[c] > caches[c].size - 8192)
            cache_used[c] = mark;
    }
    snprintf(d, sizeof(d), "%d sequences x 6 states, %d failures, %d not translated", seqs, fails, untranslated);
    check("Thumb vs reference", cache_name[c], fails == 0 && untranslated == 0, d);
    cache_used[c] = mark;
}

/* ---- step 3: speed (numbers only mean something on the board) ------------- */
#define BENCH_OPS 16   /* ALU ops per loop iteration */

/* loop(n): n iterations of BENCH_OPS dependent ALU ops on a2..a7 */
static int bench_n_ops = BENCH_OPS;
static void gen_alu_loop(xj_block_t *b)
{
    xj_emit_t *e = &b->e;
    int loop = xjb_label(b);
    xj_entry(e, 1, 32);
    xj_movi(e, 3, 1); xj_movi(e, 4, 2); xj_movi(e, 5, 3); xj_movi(e, 6, 4); xj_movi(e, 7, 5);
    xjb_bind(b, loop);
    for (int i = 0; i < bench_n_ops; i++)
        switch (i & 3)
        {
        case 0: xj_add(e, 3 + (i % 5), 3 + ((i + 1) % 5), 3 + ((i + 2) % 5)); break;
        case 1: xj_xor(e, 3 + (i % 5), 3 + ((i + 3) % 5), 3 + ((i + 1) % 5)); break;
        case 2: xj_addx2(e, 3 + (i % 5), 3 + ((i + 4) % 5), 3 + ((i + 2) % 5)); break;
        case 3: xj_sub(e, 3 + (i % 5), 3 + ((i + 2) % 5), 3 + ((i + 3) % 5)); break;
        }
    xj_addi(e, 2, 2, -1);
    xjb_bz(b, xj_bnez, 2, loop);
    xj_add(e, 2, 3, 4);
    xj_retw_n(e);
}
/* the same computation in C (kept in registers, not folded: n comes at run time) */
static uint32_t __attribute__((noinline)) c_alu_loop(int n)
{
    uint32_t r[5] = {1, 2, 3, 4, 5};
    uint32_t a = r[0], bb = r[1], c = r[2], d = r[3], f = r[4];
    uint32_t *v[5] = {&a, &bb, &c, &d, &f};
    (void)v;
    while (n--)
    {
        a = bb + c; bb = f ^ c; c = (d << 1) + a; d = a - bb;
        f = bb + c; a = d ^ f; bb = (c << 1) + d; c = f - a;
        d = a + bb; f = c ^ a; a = (bb << 1) + f; bb = d - f;
        c = f + a; d = bb ^ f; f = (a << 1) + c; a = c - d;
    }
    return a + bb;
}
/* straight(): bench_n_ops ALU ops in a row, no loop */
static void gen_straight(xj_block_t *b)
{
    xj_emit_t *e = &b->e;
    xj_entry(e, 1, 32);
    xj_movi(e, 3, 1); xj_movi(e, 4, 2); xj_movi(e, 5, 3); xj_movi(e, 6, 4); xj_movi(e, 7, 5);
    for (int i = 0; i < bench_n_ops; i++)
        switch (i & 3)
        {
        case 0: xj_add(e, 3 + (i % 5), 3 + ((i + 1) % 5), 3 + ((i + 2) % 5)); break;
        case 1: xj_xor(e, 3 + (i % 5), 3 + ((i + 3) % 5), 3 + ((i + 1) % 5)); break;
        case 2: xj_addx2(e, 3 + (i % 5), 3 + ((i + 4) % 5), 3 + ((i + 2) % 5)); break;
        case 3: xj_sub(e, 3 + (i % 5), 3 + ((i + 2) % 5), 3 + ((i + 3) % 5)); break;
        }
    xj_add(e, 2, 3, 4);
    xj_retw_n(e);
}
static void gen_tiny(xj_block_t *b)
{
    xj_entry(&b->e, 1, 32);
    xj_addi(&b->e, 2, 2, 1);
    xj_retw_n(&b->e);
}
static int helper_inc(int x) { return x + 1; }
/* calls(n): n calls of a C helper from generated code */
static void gen_calls(xj_block_t *b)
{
    xj_emit_t *e = &b->e;
    int loop = xjb_label(b);
    xj_entry(e, 1, 32);
    xj_movi(e, 3, 0);
    xjb_lit(b, 4, (uint32_t)(uintptr_t)helper_inc);
    xjb_bind(b, loop);
    xj_mov_n(e, 10, 3);
    xj_callx8(e, 4);
    xj_mov_n(e, 3, 10);
    xj_addi(e, 2, 2, -1);
    xjb_bz(b, xj_bnez, 2, loop);
    xj_mov_n(e, 2, 3);
    xj_retw_n(e);
}

static uint32_t t_start;
static void t0(void) { t_start = esp_cpu_get_cycle_count(); }
static uint32_t t1(void) { return esp_cpu_get_cycle_count() - t_start; }

static void run_bench(int c)
{
    const char *cn = cache_name[c];
    static uint8_t big[70000];
    static xj_block_t b;
    size_t mark = cache_used[c] = 0;   /* the tests' blocks are done */

    /* 1. a hot loop */
    bench_n_ops = BENCH_OPS;
    fn1_t loop = build(gen_alu_loop, c);
    if (loop)
    {
        loop(10);
        t0(); loop(10000); uint32_t cy = t1();
        printf("XJIT BENCH %-5s hot ALU loop: %.2f cycles/op (%d ops x 10000)\n", cn, cy / (BENCH_OPS * 10000.0), BENCH_OPS);
    }
    /* 2. block entry/exit */
    fn1_t tiny = build(gen_tiny, c);
    if (tiny)
    {
        int x = 0;
        t0(); for (int i = 0; i < 10000; i++) x = tiny(x); uint32_t cy = t1();
        printf("XJIT BENCH %-5s call+return of a block: %.1f cycles (x=%d)\n", cn, cy / 10000.0, x);
    }
    /* 3. calling into C from generated code */
    fn1_t calls = build(gen_calls, c);
    if (calls)
    {
        t0(); int r = calls(10000); uint32_t cy = t1();
        printf("XJIT BENCH %-5s callx8 into C and back: %.1f cycles (r=%d)\n", cn, cy / 10000.0, r);
    }
    /* 4. straight-line code bigger than the 32 KB instruction cache, run 20 times */
    for (int kb = 8; kb <= 48; kb += 40)
    {
        cache_used[c] = mark;
        xjb_init(&b, big, sizeof(big));
        bench_n_ops = kb * 1024 / 3;
        size_t room = caches[c].size - cache_used[c];
        if ((size_t)bench_n_ops * 3 + 256 > room)
        {
            printf("XJIT BENCH %-5s %d KB straight line: no room (%u bytes)\n", cn, kb, (unsigned)room);
            continue;
        }
        gen_straight(&b);
        fn1_t f = install(&b, c);
        if (!f) { printf("XJIT BENCH %-5s %d KB straight line: build failed\n", cn, kb); continue; }
        f(1);
        t0(); for (int i = 0; i < 20; i++) f(0); uint32_t cy = t1();
        printf("XJIT BENCH %-5s %d KB straight line (I-cache 32 KB): %.2f cycles/op\n", cn, kb, cy / (bench_n_ops * 20.0));
    }
    cache_used[c] = mark;
}

static void run_c_bench(void)
{
    c_alu_loop(10);
    t0(); uint32_t r = c_alu_loop(10000); uint32_t cy = t1();
    printf("XJIT BENCH C     hot ALU loop (gcc -O2): %.2f cycles/op (r=%" PRIu32 ")\n", cy / (16 * 10000.0), r);
}

void app_main(void)
{
    /* On the board this runs from a borrowed retro-go app partition: make the
       launcher the boot partition again, so the next reset goes back to it. */
    const esp_partition_t *launcher = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "launcher");
    if (launcher && esp_ota_set_boot_partition(launcher) == ESP_OK)
        printf("XJIT boot partition set back to the launcher\n");
    vTaskDelay(pdMS_TO_TICKS(3000));   /* time for the host to open the console */

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("XJIT start: model %d rev %d, internal free %u KB, PSRAM free %u KB\n", chip.model, chip.revision,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    bool ok0 = xj_exec_alloc_iram(&caches[0], 64 * 1024);
    bool ok1 = xj_exec_alloc_psram(&caches[1], 128 * 1024);
    printf("XJIT caches: IRAM %s at 0x%08" PRIx32 ", PSRAM %s at 0x%08" PRIx32 " (data %p)\n",
           ok0 ? "ok" : "FAIL", caches[0].exec, ok1 ? "ok" : "FAIL", caches[1].exec, caches[1].data);
    check("IRAM cache allocated", "-", ok0, NULL);
    check("PSRAM cache mapped", "-", ok1, NULL);

    for (int c = 0; c < 2; c++)
        if (c == 0 ? ok0 : ok1)
        {
            run_blocks(c);
            cache_used[c] = 0;
            run_thumb(c);
        }

    printf("XJIT RESULT %d/%d\n", passed, tests);

    run_c_bench();
    for (int c = 0; c < 2; c++)
        if (c == 0 ? ok0 : ok1)
            run_bench(c);
    printf("XJIT BENCH done\n");
    /* on the board: back to the launcher (set as boot partition above) */
    if (launcher)
    {
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
}
