/* m68kjit.c - block cache, translator front half and the dispatcher.
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
 * This file holds the portable C form of that ("C backend"), the model the
 * Xtensa backend must match; it is what the host fuzz test runs.
 */
#include <string.h>
#include <stdlib.h>
#include "m68kjit.h"

#define M68KJIT_MAX_INSNS 64
#define M68KJIT_MAX_BLOCKS 4096
#define M68KJIT_MAX_TOTAL (M68KJIT_MAX_BLOCKS * 16)     /* instructions over all blocks */
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
    int first, count;               /* in insns[] */
    int next_in_bucket;             /* -1 at the end */
} block_t;

static m68kjit_host_t H;
static block_t *blocks;
static insn_t *insns;
static int n_blocks, n_insns;
static int bucket[1 << HASH_BITS];
m68kjit_stats_t m68kjit_stats;

bool m68kjit_init(const m68kjit_host_t *host)
{
    H = *host;
    if (!blocks) blocks = malloc(sizeof(block_t) * M68KJIT_MAX_BLOCKS);
    if (!insns) insns = malloc(sizeof(insn_t) * M68KJIT_MAX_TOTAL);
    if (!blocks || !insns) return false;
    m68kjit_flush();
    return true;
}

void m68kjit_flush(void)
{
    n_blocks = n_insns = 0;
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

static int translate(uint32_t pc)
{
    if (n_blocks == M68KJIT_MAX_BLOCKS || n_insns + M68KJIT_MAX_INSNS > M68KJIT_MAX_TOTAL)
        m68kjit_flush();
    block_t *b = &blocks[n_blocks];
    b->pc = pc;
    b->first = n_insns;
    b->count = 0;
    while (b->count < M68KJIT_MAX_INSNS && H.is_code(pc, 2))
    {
        uint16_t op = H.read_code16(pc);
        int len = m68kjit_insn_len(op);
        if (!H.is_code(pc, len))
            break;
        insn_t *i = &insns[n_insns++];
        i->handler = H.handlers[op];
        i->pc = pc;
        i->next = pc + len;
        i->op = op;
        i->cyc = H.cyc[op];
        b->count++;
        pc += len;
        if (ends_block(op))
            break;
    }
    if (b->count == 0)
        return -1;
    int h = HASH(b->pc);
    b->next_in_bucket = bucket[h];
    bucket[h] = n_blocks;
    m68kjit_stats.blocks++;
    m68kjit_stats.insns += b->count;
    return n_blocks++;
}

static void run_block(const block_t *b)
{
    const insn_t *i = &insns[b->first], *end = i + b->count;
    m68kjit_stats.block_runs++;
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
}

void m68kjit_run(void)
{
    do
    {
        uint32_t pc = *H.pc;
        int b = lookup(pc);
        if (b < 0 && H.is_code(pc, 2))
            b = translate(pc);
        if (b >= 0)
            run_block(&blocks[b]);
        else
        {
            m68kjit_stats.steps++;
            H.step();
        }
    } while (*H.cycles > 0);
}
