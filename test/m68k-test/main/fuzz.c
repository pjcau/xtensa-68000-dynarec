/* fuzz.c - differential test: the same random machine run by Musashi's
 * m68k_execute() and by the dynarec (glue_jit_execute()), compared after
 * every time slice.
 *
 * Memory: 1 MB mirrored over the 24-bit bus. 0x00000-0x3FFFF is ROM (writes
 * ignored; the only code the dynarec translates), the rest RAM. The ROM is
 * random words, so programs are random instruction streams: every opcode
 * valid on the 68000, illegal ones (exceptions), branches anywhere. The
 * vector table points into the ROM. Between slices the interrupt level
 * changes at random.
 *
 * Built against Musashi 4.5 (vendored) or, with FUZZ_MUSASHI31, against
 * mame-go's Musashi 3.1 (see ../musashi31/README.md): then change_pc32() calls
 * are counted and compared too, and the MAMEGO idle-loop skip runs, with an I/O
 * window at 0xF0000-0xFFFFF.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m68k.h"
#include "m68kjit.h"
#include "fuzz.h"

#define ROM_SIZE 0x40000
#define MEM_SIZE 0x100000
#define MASK(a) ((a) & (MEM_SIZE - 1))

static uint8_t *mem, *snap_mem;
/* Musashi globals outside the context m68k_get_context() saves */
extern int m68ki_remaining_cycles, m68ki_initial_cycles;
static uint8_t snap_ctx[4096];

static uint32_t rng;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

unsigned int m68k_read_memory_8(unsigned int a) { return mem[MASK(a)]; }
unsigned int m68k_read_memory_16(unsigned int a) { return mem[MASK(a)] << 8 | mem[MASK(a + 1)]; }
unsigned int m68k_read_memory_32(unsigned int a) { return m68k_read_memory_16(a) << 16 | m68k_read_memory_16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { if (MASK(a) >= ROM_SIZE) mem[MASK(a)] = v; }
void m68k_write_memory_16(unsigned int a, unsigned int v) { m68k_write_memory_8(a, v >> 8); m68k_write_memory_8(a + 1, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v); }
unsigned int fuzz_read16(unsigned int a) { return m68k_read_memory_16(a); }
#ifdef FUZZ_MUSASHI31
/* (3.1's m68kmame.h maps the disassembler reads onto the memory calls above;
 * the generator's instruction check comes from ../musashi31/shim_dasm.c) */
static uint32_t pcc_count, pcc_last;
int glue_hot_threshold;
void fuzz31_change_pc(unsigned int pc) { pcc_count++; pcc_last = pc; }
void fuzz31_idle_reset(void);
extern unsigned int m68ki_idle_enable, m68ki_idle_io_lo, m68ki_idle_io_hi;
#else
unsigned int m68k_read_disassembler_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }
static uint32_t pcc_count, pcc_last;
#endif

bool fuzz_translate = true;         /* false: the dynarec interprets everything (harness check) */
/* percent of the generated instructions drawn from the forms m68kjit
 * translates natively (register-only ALU, MOVE, MOVEQ, ADDQ, LEA, ...) */
int fuzz_native_bias = 0;
int fuzz_form = -1;                 /* >= 0: native_form() draws only this family */

static uint16_t native_form(void)
{
    uint32_t r = rnd();
    int rx = (r >> 4) & 7, ry = (r >> 7) & 7, ss = (r >> 10) % 3;
    int smode = (int[]){0, 1, 7}[(r >> 12) % 3], sreg = smode == 7 ? 4 : ry;
    int mmode = (int[]){2, 3, 4, 5, 6, 7, 7}[(r >> 25) % 7], mreg = mmode == 7 ? (r >> 28) % 2 : rx;   /* (An) (An)+ -(An) (d16,An) (d8,An,Xn) abs.w abs.l */
    int cmode = (int[]){2, 5, 6, 7, 7, 7, 7}[(r >> 25) % 7], creg = cmode == 7 ? (r >> 28) % 4 : rx;   /* control modes, PC-relative too */
    switch (fuzz_form >= 0 ? fuzz_form : (int)((r >> 16) % 23))   /* 22: Bcc (default) */
    {
    case 0: return 0x7000 | rx << 9 | (r >> 20 & 0xFF);                                   /* MOVEQ */
    case 1: return (int[]){0x1000, 0x3000, 0x2000}[ss] | rx << 9 | ((r >> 20) & 1) << 6 | smode << 3 | sreg;   /* MOVE/MOVEA */
    case 2: return 0x5000 | rx << 9 | ((r >> 20) & 1) << 8 | ss << 6 | ((r >> 21) & 1) << 3 | ry;         /* ADDQ/SUBQ */
    case 3: case 4: case 5:
        return (int[]){0x8000, 0x9000, 0xB000, 0xC000, 0xD000}[(r >> 20) % 5] | rx << 9 | ss << 6 | smode << 3 | sreg;  /* OR SUB CMP AND ADD */
    case 6: return (int[]){0x9000, 0xB000, 0xD000}[(r >> 20) % 3] | rx << 9 | ((r >> 22) & 1 ? 7 : 3) << 6 | smode << 3 | sreg;   /* SUBA CMPA ADDA */
    case 7: return 0xB100 | rx << 9 | ss << 6 | ry;                                         /* EOR Dn,Dm */
    case 8: return ((r >> 20) & 1 ? 0x4A00 : 0x4200) | ss << 6 | ry;                        /* TST, CLR */
    case 9: return (int[]){0x4840, 0x4880, 0x48C0}[(r >> 20) % 3] | ry;                    /* SWAP EXT */
    case 10: return (int[]){0x41C0 | rx << 9, 0x4840, 0x4E80}[(r >> 20) % 3] | cmode << 3 | creg;   /* LEA PEA JSR */
    case 18: return (r >> 20) & 1 ? 0x4E75 : 0x6100 | (r >> 21 & 0xFE);                     /* RTS, BSR */
    case 19: return (int[]){0x1000, 0x3000, 0x2000}[ss] | rx << 9 | ((r >> 22) & 1) << 6 | 7 << 3 | (2 + (r >> 23) % 2);    /* MOVE (d16,PC)/(d8,PC,Xn) -> reg */
    case 12: return (int[]){0x1000, 0x3000, 0x2000}[ss] | (mmode == 7 ? mreg : ry) << 9 | mmode << 6 | smode << 3 | sreg;   /* MOVE reg/imm -> mem */
    case 13: return (int[]){0x1000, 0x3000, 0x2000}[ss] | rx << 9 | ((r >> 22) & 1) << 6 | mmode << 3 | mreg;                  /* MOVE mem -> reg */
    case 14: return (int[]){0x8000, 0x9000, 0xB000, 0xC000, 0xD000}[(r >> 20) % 5] | rx << 9 | ((r >> 23) & 1) << 8 | ss << 6 | mmode << 3 | mreg;   /* ALU mem */
    case 15: return (int[]){0x0000, 0x0200, 0x0400, 0x0600, 0x0A00, 0x0C00}[(r >> 20) % 6] | ss << 6 | ((r >> 23) & 1 ? mmode << 3 | mreg : ry);  /* xxxI */
    case 16: return ((r >> 20) & 1 ? 0x4A00 : 0x4200) | ss << 6 | mmode << 3 | mreg;        /* TST/CLR mem */
    case 17: return 0x5000 | rx << 9 | ((r >> 20) & 1) << 8 | ss << 6 | mmode << 3 | mreg;  /* ADDQ/SUBQ mem */
    case 20: return 0xE000 | rx << 9 | ((r >> 20) & 1) << 8 | ss << 6 | ((r >> 21) & 7) << 3 | ry;   /* shifts: #n and Dn counts, all kinds */
    case 21: return 0x4880 | ((r >> 20) & 1) << 10 | ((r >> 21) & 1) << 6 | (int[]){2, 3, 4, 5, 6, 7, 7}[(r >> 22) % 7] << 3 | ((r >> 22) % 7 >= 5 ? (r >> 25) % 4 : ry);   /* MOVEM */
    case 11: return 0x50C8 | ((r >> 20) & 0xF) << 8 | ry;                                   /* DBcc (disp: next word) */
    default: return 0x6000 | ((r >> 20) & 0xF) << 8 | (r >> 24 & 0xFE);                   /* Bcc: flags get used */
    }
}

static bool is_code(uint32_t a, int len) { return fuzz_translate && a < ROM_SIZE && a + len <= ROM_SIZE; }
static uint16_t read_code16(uint32_t a) { return m68k_read_memory_16(a); }

typedef struct
{
    uint32_t reg[20];               /* D0-D7, A0-A7, PC, SR, USP, ISP */
    int32_t ret, left;
    uint32_t ram_hash;
    uint32_t pcc_count, pcc_last;   /* change_pc32() calls (Musashi 3.1) */
} state_t;

static const m68k_register_t regs[20] = {
    M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3, M68K_REG_D4, M68K_REG_D5, M68K_REG_D6, M68K_REG_D7,
    M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3, M68K_REG_A4, M68K_REG_A5, M68K_REG_A6, M68K_REG_A7,
    M68K_REG_PC, M68K_REG_SR, M68K_REG_USP, M68K_REG_ISP,
};
static const char *reg_names[20] = {"D0","D1","D2","D3","D4","D5","D6","D7","A0","A1","A2","A3","A4","A5","A6","A7","PC","SR","USP","ISP"};

static void capture(state_t *s, int ret)
{
    for (int i = 0; i < 20; i++) s->reg[i] = m68k_get_reg(NULL, regs[i]);
    s->ret = ret;
    s->left = m68k_cycles_remaining();
    uint32_t h = 2166136261u;
    for (int i = ROM_SIZE; i < MEM_SIZE; i++) h = (h ^ mem[i]) * 16777619u;
    s->ram_hash = h;
    s->pcc_count = pcc_count;
    s->pcc_last = pcc_last;
}

static void setup(uint32_t seed)
{
    rng = seed * 2654435761u + 1;
    for (int i = 0; i < MEM_SIZE; i += 2) { uint32_t r = rnd(); mem[i] = r; mem[i + 1] = r >> 8; }
    /* vectors: SSP in RAM, everything else (reset PC too) into the ROM past the table */
    for (int v = 0; v < 256; v++)
    {
        uint32_t a = v == 0 ? ROM_SIZE + (rnd() % (MEM_SIZE - ROM_SIZE)) : 0x400 + (rnd() % (ROM_SIZE - 0x400));
        a &= ~1u;
        mem[v * 4] = a >> 24; mem[v * 4 + 1] = a >> 16; mem[v * 4 + 2] = a >> 8; mem[v * 4 + 3] = a;
    }
    /* most of the ROM: whole instructions valid on the 68000 that do not trap
     * by themselves, so blocks run long; the rest stays random words */
    for (uint32_t pc = 0x400; pc + 16 < ROM_SIZE;)
    {
        if (rnd() % 8 == 0) { pc += 2 * (1 + rnd() % 4); continue; }
        if (rnd() % 64 == 0)
        {   /* a wait loop: BTST #0,(4,PC); BEQ.S back; on a zero byte it spins
             * until an interrupt, which is what mame-go's idle-loop skip detects */
            static const uint8_t loop[10] = {0x08, 0x3A, 0x00, 0x00, 0x00, 0x04, 0x67, 0xF8, 0x00, 0x00};
            memcpy(&mem[pc], loop, 10);
            pc += 10;
            continue;
        }
        uint16_t op;
        bool nat = (int)(rnd() % 100) < fuzz_native_bias;
        do op = nat ? native_form() : rnd(); while (!m68k_is_valid_instruction(op, M68K_CPU_TYPE_68000) || (op >> 12) == 0xA || (op >> 12) == 0xF
                             || op == 0x4AFC || (op & 0xFFF0) == 0x4E40 || op == 0x4E72 || op == 0x4E70);
        mem[pc] = op >> 8; mem[pc + 1] = op;
        int len = m68kjit_insn_len(op);
        for (int i = 2; i < len; i += 2) mem[pc + i] &= ~1;     /* extension words: brief format (bit 8 clear) */
        pc += len;
        /* native-heavy: often save SR on the stack (RAM) right after, so the RAM
         * hash sees the flags of that instruction before later ones overwrite them */
        if (nat && (rnd() & 1)) { mem[pc] = 0x40; mem[pc + 1] = 0xE7; pc += 2; }   /* MOVE SR,-(A7) */
    }
    m68k_pulse_reset();
    static const uint32_t edge[] = {0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0xFF, 0x80, 0x7F,
                                    0xFFFF, 0x8000, 0x7FFF, 0xFFFFFF80, 0xFFFF8000, 0x00FF00FF, 0xFF00FF00};
    for (int i = 0; i < 15; i++)
    {
        uint32_t r = rnd();
        if (i < 8 && (rnd() & 1)) r = edge[rnd() % (sizeof(edge) / sizeof(edge[0]))];   /* flag edge cases */
        if (i >= 8 && (r & 3)) r = ROM_SIZE + (r % (MEM_SIZE - ROM_SIZE));   /* most An point into RAM */
        m68k_set_reg(regs[i], r);
    }
    m68k_set_reg(M68K_REG_SR, 0x2000 | (rnd() & 0x071F));
    m68k_set_reg(M68K_REG_USP, ROM_SIZE + (rnd() % (MEM_SIZE - ROM_SIZE)));
}

int fuzz_init(void)
{
    mem = fuzz_alloc(MEM_SIZE);
    snap_mem = fuzz_alloc(MEM_SIZE);
    if (!mem || !snap_mem || m68k_context_size() > sizeof(snap_ctx)) return -1;
#ifdef FUZZ_MUSASHI31
#ifndef FUZZ_NO_IDLE
    m68ki_idle_enable = 1;
#endif
    m68ki_idle_io_lo = 0xF0000;
    m68ki_idle_io_hi = 0xFFFFF;
#else
    m68k_init();
#endif
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    return glue_jit_init(is_code, read_code16) ? 0 : -1;
}

/* one seed: returns 0 if both engines agree after every slice */
int fuzz_seed(uint32_t seed, int slices, bool verbose)
{
    static state_t a[64], b[64];
    int irq[64], cyc[64];
    if (slices > 64) slices = 64;

    setup(seed);
    for (int s = 0; s < slices; s++) { irq[s] = (rnd() % 4) ? 0 : rnd() % 8; cyc[s] = 1 + rnd() % 2000; }
    m68k_get_context(snap_ctx);
    memcpy(snap_mem, mem, MEM_SIZE);

    m68ki_remaining_cycles = m68ki_initial_cycles = 0;
    pcc_count = pcc_last = 0;
#ifdef FUZZ_MUSASHI31
    fuzz31_idle_reset();
#endif
    for (int s = 0; s < slices; s++) { m68k_set_irq(irq[s]); capture(&a[s], m68k_execute(cyc[s])); }

    m68k_set_context(snap_ctx);
    memcpy(mem, snap_mem, MEM_SIZE);
    glue_jit_init(is_code, read_code16);     /* (flushes; picks up glue_hot_threshold) */
    m68ki_remaining_cycles = m68ki_initial_cycles = 0;
    pcc_count = pcc_last = 0;
#ifdef FUZZ_MUSASHI31
    fuzz31_idle_reset();
#endif
    for (int s = 0; s < slices; s++) { m68k_set_irq(irq[s]); capture(&b[s], glue_jit_execute(cyc[s])); }

    for (int s = 0; s < slices; s++)
    {
        if (!memcmp(&a[s], &b[s], sizeof(state_t))) continue;
        if (verbose)
        {
            printf("MISMATCH seed %u slice %d (cycles %d, irq %d):", (unsigned)seed, s, cyc[s], irq[s]);
            for (int i = 0; i < 20; i++)
                if (a[s].reg[i] != b[s].reg[i]) printf(" %s %08X/%08X", reg_names[i], (unsigned)a[s].reg[i], (unsigned)b[s].reg[i]);
            if (a[s].ret != b[s].ret) printf(" ret %d/%d", (int)a[s].ret, (int)b[s].ret);
            if (a[s].left != b[s].left) printf(" left %d/%d", (int)a[s].left, (int)b[s].left);
            if (a[s].ram_hash != b[s].ram_hash) printf(" ram");
            if (a[s].pcc_count != b[s].pcc_count || a[s].pcc_last != b[s].pcc_last)
                printf(" change_pc %u:%06X/%u:%06X", (unsigned)a[s].pcc_count, (unsigned)a[s].pcc_last, (unsigned)b[s].pcc_count, (unsigned)b[s].pcc_last);
            printf("  (interpreter/jit)\n");
        }
        return 1;
    }
    return 0;
}
