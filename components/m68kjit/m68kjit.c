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
static uint32_t base;                       /* the lowest of &PPC, &PC, &IR */
static int o_ppc, o_pc, o_ir;               /* their offsets from base */
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
    /* PPC, PC and IR are reached with s32i from one base register: they must
     * sit within 1020 bytes of each other (they do in every Musashi) */
    uint32_t p = (uintptr_t)H.ppc, c = (uintptr_t)H.pc, i = (uintptr_t)H.ir;
    base = p < c ? p : c;
    if (i < base) base = i;
    o_ppc = p - base; o_pc = c - base; o_ir = i - base;
    if (((p | c | i) & 3) || o_ppc > 1020 || o_pc > 1020 || o_ir > 1020)
        return false;
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
/* The block, as Xtensa (windowed ABI, called with callx8 from m68kjit_run):
 *
 *     entry a1, 32
 *     a2 = base (&PPC/&PC/&IR), a3 = &cycles, a5 = pc of the instruction
 *   per instruction:
 *     s32i a5, a2, PPC;  addi a7, a5, 2;  s32i a7, a2, PC
 *     a6 = op;  s32i a6, a2, IR
 *     a8 = handler;  callx8 a8               (a2..a7 survive the call)
 *     l32i a6, a3, 0;  a6 -= cyc;  s32i a6, a3, 0
 *     l32i a7, a2, PC;  addi a5, a5, len
 *     beq a7, a5, 1f;  retw.n;  1:           leave on a PC change
 *     bgei a6, 1, 2f;  retw.n;  2:           leave when the slice is over
 *   retw.n
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
        xj_s32i(e, 5, 2, o_ppc);
        xj_addi(e, 7, 5, 2);
        xj_s32i(e, 7, 2, o_pc);
        xjb_imm(b, 6, i->op);
        xj_s32i(e, 6, 2, o_ir);
        xjb_lit(b, 8, (uint32_t)(uintptr_t)i->handler);
        xj_callx8(e, 8);
        xj_l32i(e, 6, 3, 0);
        if (i->cyc <= 128)
            xj_addi(e, 6, 6, -(int)i->cyc);
        else
        {
            xj_movi(e, 7, i->cyc);
            xj_sub(e, 6, 6, 7);
        }
        xj_s32i(e, 6, 3, 0);
        if (k == n - 1)
            break;                          /* the last one leaves anyway */
        xj_l32i(e, 7, 2, o_pc);
        xj_addi(e, 5, 5, i->next - i->pc);
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
