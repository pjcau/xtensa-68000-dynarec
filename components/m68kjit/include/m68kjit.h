/* m68kjit.h - 68000 dynarec on xjit, on top of a Musashi interpreter.
 *
 * Blocks of 68000 instructions are translated into Xtensa code. An
 * instruction the translator does not handle natively becomes a call to the
 * interpreter's own handler for that opcode, so every instruction is
 * supported from the start and the result matches the interpreter: after
 * each instruction the block leaves if the program counter is not the next
 * one (a taken branch, an exception, an interrupt) or the time slice is
 * over, exactly where the interpreter's loop would stop.
 *
 * The interpreter is reached only through m68kjit_host_t, filled by a small
 * glue file compiled against the emulator's own Musashi (its version, its
 * configuration): see test/m68k-test/main/glue_musashi.c.
 */
#ifndef M68KJIT_H
#define M68KJIT_H

#include <stdint.h>
#include <stdbool.h>

typedef void (*m68kjit_handler_t)(void);

typedef struct
{
    /* interpreter state the blocks read and write */
    uint32_t *pc, *ppc, *ir;
    uint32_t *dar;                          /* D0-D7 then A0-A7 */
    /* flags in Musashi's form: X and C in bit 8, N and V in bit 7, Z = 0 when set */
    uint32_t *flag_x, *flag_n, *flag_z, *flag_v, *flag_c;
    int32_t *cycles;                        /* cycles left in the time slice */
    const m68kjit_handler_t *handlers;      /* opcode -> handler (65536) */
    const uint8_t *cyc;                     /* opcode -> base cycles (65536) */
    /* cycle adjustments of the CPU type (Musashi's USE_CYCLES(CYC_...), may be < 0) */
    int cyc_bcc_notake_b, cyc_bcc_notake_w, cyc_dbcc_f_noexp, cyc_dbcc_f_exp;
    int cyc_shift;                          /* a shift by n costs n << cyc_shift more... */
    bool shift_imm_plain;                   /* ...but not a shift by #n (Musashi 3.1) */
    int cyc_movem_w, cyc_movem_l;           /* MOVEM: registers << these, more */
    bool dbf_plain;
    /* a memory access can take an interrupt at once (Musashi 3.1's m68k_set_irq()
     * from a write handler): every flag must be up to date before one */
    bool mem_may_interrupt;                         /* DBF adds no cyc_dbcc_f_* (Musashi 3.1's m68k_op_dbf_16) */
    /* MOVE.l / MOVEM.l to -(An) as two 16-bit writes, low word first (Musashi 4.x) */
    bool pd_long_split16;
    /* memory as the interpreter's instructions see it (address mask, function code) */
    uint32_t (*read8)(uint32_t), (*read16)(uint32_t), (*read32)(uint32_t);
    void (*write8)(uint32_t, uint32_t), (*write16)(uint32_t, uint32_t), (*write32)(uint32_t, uint32_t);
    /* optional hooks of the emulator's Musashi (NULL when it has none):
     * branch_back: after a taken branch that goes back by 1..32 bytes, with PPC
     *   and PC set, before its cycles (mame-go's idle-loop check, which may
     *   give the rest of the time slice away);
     * pc_changed: after a jump (JSR, RTS), as m68ki_jump() does */
    void (*branch_back)(void);
    void (*pc_changed)(uint32_t pc);
    /* bytes of PSRAM for translated code (0: 1 MB); fixed by the first init */
    uint32_t code_size;
    /* translate a block only after its address was reached this many times
     * without one (0: at once); cold code stays with the interpreter */
    int hot_threshold;
    /* one interpreter step (fetch, handler, cycles) for code not translated */
    void (*step)(void);
    /* code the translator may read: true if [addr, addr + len) is fixed code (ROM) */
    bool (*is_code)(uint32_t addr, int len);
    uint16_t (*read_code16)(uint32_t addr);
} m68kjit_host_t;

bool m68kjit_init(const m68kjit_host_t *host);
/* run translated blocks (or single interpreted steps) while cycles are left:
 * the body of m68k_execute()'s loop, called by the glue's execute */
void m68kjit_run(void);
void m68kjit_flush(void);                   /* drop every translated block */

typedef struct
{
    uint32_t blocks, insns, native, links, block_runs, block_insns, steps, exits_pc, exits_cycles, flushes, code_bytes;
} m68kjit_stats_t;
extern m68kjit_stats_t m68kjit_stats;

/* length in bytes of the 68000 instruction starting with this word */
int m68kjit_insn_len(uint16_t op);

#endif
