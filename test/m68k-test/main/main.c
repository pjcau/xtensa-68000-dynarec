/* m68k-test: the differential fuzz (fuzz.c) with the Xtensa backend of
 * m68kjit, in QEMU or on the board. Prints "M68K ..." lines. */
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "m68kjit.h"
#include "fuzz.h"

void *fuzz_alloc(int size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }

void app_main(void)
{
    if (fuzz_init()) { printf("M68K init failed\n"); return; }
    printf("M68K init ok\n");
    /* the harness alone first: every instruction interpreted by the dynarec's loop */
    fuzz_translate = false;
    int hbad = 0;
    for (int i = 0; i < 20; i++) hbad += fuzz_seed(1 + i, 32, true);
    printf("M68K harness (no translation) 20 seeds, %d mismatches\n", hbad);
    fuzz_translate = true;
    int n = 300, bad = 0;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < n; i++)
    {
        if (fuzz_seed(1 + i, 32, bad < 20)) bad++;
        if ((i + 1) % 50 == 0) printf("M68K %d seeds, %d mismatches\n", i + 1, bad);
    }
    printf("M68K FUZZ %d seeds, %d mismatches; blocks %u insns %u runs %u steps %u (%lld ms)\n",
           n, bad, (unsigned)m68kjit_stats.blocks, (unsigned)m68kjit_stats.insns, (unsigned)m68kjit_stats.block_runs,
           (unsigned)m68kjit_stats.steps, (esp_timer_get_time() - t0) / 1000);
    printf("M68K done\n");
}
