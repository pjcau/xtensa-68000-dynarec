/* Musashi 3.1's m68kcpu.c, plus a reset of its (static) idle-loop state:
 * both runs of a fuzz seed must start from the same state */
#include "m68kcpu.c"

void fuzz31_idle_reset(void)
{
#ifdef MAMEGO
    idle_pc = idle_whash = idle_count = 0;
    memset(idle_regs, 0, sizeof(idle_regs));
    m68ki_idle_whash = m68ki_idle_io = 0;
#endif
}

/* host runs with IDLESTAT=1: how many idle-loop skips the statistics recorded
 * (a lower bound: its table keeps one loop per slot) - to see the fuzz
 * exercises the skip */
unsigned int fuzz31_idle_skips(void)
{
    unsigned int n = 0;
#if defined(MAMEGO) && !defined(ESP_PLATFORM)
    for (int i = 0; i < 256; i++) n += idle_stats[i].idle;
#endif
    return n;
}
