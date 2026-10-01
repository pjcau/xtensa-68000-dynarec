/* glue_musashi31.c - m68kjit on the Musashi 3.1 of mame-go (MAME 0.37b5 with
 * the mame-go changes): the host table and m68k_execute() with its loop body
 * handed to m68kjit_run().
 *
 * Compiled by the emulator, against its own Musashi (this file only includes
 * its headers). What this Musashi does differently from 4.x, and the host
 * table carries:
 *  - MOVE.l / MOVEM.l to -(An) write one 32-bit value (pd_long_split16 = false);
 *  - DBF (DBRA) adds none of the DBcc cycle adjustments (dbf_plain);
 *  - a shift or rotate by #n adds no per-bit cycles (shift_imm_plain);
 *  - m68k_set_irq() takes the interrupt at once, also from inside a memory
 *    handler: the flags must be current before every access (mem_may_interrupt);
 *  - M68K_MONITOR_PC: a jump calls change_pc32() (pc_changed);
 *  - MAMEGO: the generic idle-loop skip, checked on short backward branches
 *    (branch_back), fed by the write hash in m68ki_write_*() (the native code
 *    writes through those, so the hash is the same);
 *  - m68k_execute() spends the interrupt cycles (CPU_INT_CYCLES) around the loop.
 */
#include "m68kcpu.h"
#include "m68kops.h"
#include "m68kjit.h"

extern int m68ki_initial_cycles;

/* interpreted steps per 1 MB of the 24-bit bus: where the dynarec does not run */
uint32_t glue31_steps_by_mb[16];

static void step(void)
{
    glue31_steps_by_mb[(REG_PC >> 20) & 15]++;
    REG_PPC = REG_PC;
    REG_IR = m68ki_read_imm_16();
    m68ki_instruction_jump_table[REG_IR]();
    USE_CYCLES(CYC_INSTRUCTION[REG_IR]);
}

static uint32_t rd8(uint32_t a) { return m68ki_read_8(a); }
static uint32_t rd16(uint32_t a) { return m68ki_read_16(a); }
static uint32_t rd32(uint32_t a) { return m68ki_read_32(a); }
static void wr8(uint32_t a, uint32_t v) { m68ki_write_8(a, v); }
static void wr16(uint32_t a, uint32_t v) { m68ki_write_16(a, v); }
static void wr32(uint32_t a, uint32_t v) { m68ki_write_32(a, v); }
static void pc_changed(uint32_t pc) { (void)pc; m68ki_pc_changed(pc); }
#ifdef MAMEGO
static void branch_back(void) { M68KI_IDLE_CHECK(); }
#endif

bool glue31_jit_init(bool (*is_code)(uint32_t, int), uint16_t (*read_code16)(uint32_t), uint32_t code_size, int hot_threshold)
{
    m68kjit_host_t h = {
        .pc = (uint32_t *)&REG_PC,
        .ppc = (uint32_t *)&REG_PPC,
        .ir = (uint32_t *)&REG_IR,
        .dar = (uint32_t *)REG_DA,
        .flag_x = (uint32_t *)&FLAG_X, .flag_n = (uint32_t *)&FLAG_N, .flag_z = (uint32_t *)&FLAG_Z,
        .flag_v = (uint32_t *)&FLAG_V, .flag_c = (uint32_t *)&FLAG_C,
        .cycles = (int32_t *)&m68ki_remaining_cycles,
        .handlers = (const m68kjit_handler_t *)m68ki_instruction_jump_table,
        .cyc = CYC_INSTRUCTION,
        .cyc_bcc_notake_b = (int)CYC_BCC_NOTAKE_B, .cyc_bcc_notake_w = (int)CYC_BCC_NOTAKE_W,
        .cyc_dbcc_f_noexp = (int)CYC_DBCC_F_NOEXP, .cyc_dbcc_f_exp = (int)CYC_DBCC_F_EXP,
        .cyc_shift = (int)CYC_SHIFT,
        .cyc_movem_w = (int)CYC_MOVEM_W, .cyc_movem_l = (int)CYC_MOVEM_L,
        .pd_long_split16 = false,
        .dbf_plain = true,
        .mem_may_interrupt = true,
        .shift_imm_plain = true,
        .read8 = rd8, .read16 = rd16, .read32 = rd32,
        .write8 = wr8, .write16 = wr16, .write32 = wr32,
#ifdef MAMEGO
        .branch_back = branch_back,
#endif
        .pc_changed = pc_changed,
        .step = step,
        .is_code = is_code,
        .read_code16 = read_code16,
        .code_size = code_size,
        .hot_threshold = hot_threshold,
    };
    return m68kjit_init(&h);
}

/* m68k_execute() of mame-go's Musashi 3.1 with the dynarec in the loop */
int glue31_jit_execute(int num_cycles)
{
    if (!CPU_STOPPED)
    {
        SET_CYCLES(num_cycles);
        m68ki_initial_cycles = num_cycles;
        USE_CYCLES(CPU_INT_CYCLES);
        CPU_INT_CYCLES = 0;
        m68kjit_run();
        REG_PPC = REG_PC;
        USE_CYCLES(CPU_INT_CYCLES);
        CPU_INT_CYCLES = 0;
        return m68ki_initial_cycles - GET_CYCLES();
    }
    SET_CYCLES(0);
    CPU_INT_CYCLES = 0;
    return num_cycles;
}
