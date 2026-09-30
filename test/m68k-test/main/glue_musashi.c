/* glue_musashi.c - m68kjit on Musashi 4.5: the host table and an execute
 * that is m68k_execute() with the loop body handed to m68kjit_run().
 *
 * An emulator with another Musashi (mame-go: 3.1, gwenesis: 3.32) gets its
 * own copy of this file, written against its m68kcpu.h and its m68k_execute.
 * Blocks never copy REG_DA to REG_DA_SAVE: that copy only serves the bus
 * error of the 68030 MMU, which a 68000 does not have.
 */
#include "m68kcpu.h"
#include "m68kops.h"
#include "m68kjit.h"

extern int m68ki_initial_cycles;         /* m68kcpu.c, not in its header */

static void step(void)
{
    REG_PPC = REG_PC;
    REG_IR = m68ki_read_imm_16();
    m68ki_instruction_jump_table[REG_IR]();
    USE_CYCLES(CYC_INSTRUCTION[REG_IR]);
}

bool glue_jit_init(bool (*is_code)(uint32_t, int), uint16_t (*read_code16)(uint32_t))
{
    m68kjit_host_t h = {
        .pc = (uint32_t *)&REG_PC,
        .ppc = (uint32_t *)&REG_PPC,
        .ir = (uint32_t *)&REG_IR,
        .cycles = (int32_t *)&m68ki_remaining_cycles,
        .handlers = (const m68kjit_handler_t *)m68ki_instruction_jump_table,
        .cyc = CYC_INSTRUCTION,
        .step = step,
        .is_code = is_code,
        .read_code16 = read_code16,
    };
    return m68kjit_init(&h);
}

/* m68k_execute() of Musashi 4.5 with the dynarec in the loop */
int glue_jit_execute(int num_cycles)
{
    if (RESET_CYCLES)
    {
        int rc = RESET_CYCLES;
        RESET_CYCLES = 0;
        num_cycles -= rc;
        if (num_cycles <= 0)
            return rc;
    }
    SET_CYCLES(num_cycles);
    m68ki_initial_cycles = num_cycles;
    m68ki_check_interrupts();
    if (!CPU_STOPPED)
    {
        m68ki_set_address_error_trap();
        m68ki_check_bus_error_trap();
        m68kjit_run();
        REG_PPC = REG_PC;
    }
    else
        SET_CYCLES(0);
    return m68ki_initial_cycles - GET_CYCLES();
}
