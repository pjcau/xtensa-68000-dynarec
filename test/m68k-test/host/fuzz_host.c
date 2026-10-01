/* fuzz_host: the differential test on the PC, with the C backend.
 *   ./fuzz_host [seeds] [first seed] [native bias %] [hot threshold] */
#include <stdio.h>
#include <stdlib.h>
#include "m68kjit.h"
#include "fuzz.h"

void *fuzz_alloc(int size) { return malloc(size); }

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1000;
    uint32_t first = argc > 2 ? strtoul(argv[2], NULL, 0) : 1;
    if (fuzz_init()) { printf("init failed\n"); return 2; }
    if (argc > 3) fuzz_native_bias = atoi(argv[3]);
    if (argc > 4) glue_hot_threshold = atoi(argv[4]);
    int bad = 0;
    for (int i = 0; i < n; i++)
        if (fuzz_seed(first + i, 32, bad < 20)) bad++;
    printf("FUZZ %d seeds, %d mismatches; blocks %u insns %u runs %u (%u insns) steps %u exits pc %u cycles %u\n",
           n, bad, m68kjit_stats.blocks, m68kjit_stats.insns, m68kjit_stats.block_runs, m68kjit_stats.block_insns,
           m68kjit_stats.steps, m68kjit_stats.exits_pc, m68kjit_stats.exits_cycles);
#ifdef FUZZ_MUSASHI31
    { extern unsigned int fuzz31_idle_skips(void); printf("FUZZ idle-loop skips seen (IDLESTAT=1): %u\n", fuzz31_idle_skips()); }
#endif
    return bad != 0;
}
