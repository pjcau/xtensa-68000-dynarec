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
#ifdef FUZZ_FORMS_ONLY
    n = 0;
#endif
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < n; i++)
    {
        if (fuzz_seed(1 + i, 32, bad < 20)) bad++;
        if ((i + 1) % 50 == 0) printf("M68K %d seeds, %d mismatches\n", i + 1, bad);
    }
    printf("M68K FUZZ %d seeds, %d mismatches; blocks %u insns %u native %u runs %u steps %u (%lld ms)\n",
           n, bad, (unsigned)m68kjit_stats.blocks, (unsigned)m68kjit_stats.insns, (unsigned)m68kjit_stats.native, (unsigned)m68kjit_stats.block_runs,
           (unsigned)m68kjit_stats.steps, (esp_timer_get_time() - t0) / 1000);
    {
        char bytes[160];
        m68kjit_stats_bytes(bytes, sizeof bytes);
        printf("M68K BYTES/INSN %s (code %u KB)\n", bytes, (unsigned)(m68kjit_stats.code_bytes >> 10));
    }
    /* the same with the ROM mostly made of the natively translated forms */
    fuzz_native_bias = 85;
    bad = 0;
#ifdef FUZZ_FORMS_ONLY
    n = 0;
#endif
    uint32_t nat0 = m68kjit_stats.native, runs0 = m68kjit_stats.block_runs;
    for (int i = 0; i < n; i++)
        if (fuzz_seed(1001 + i, 32, bad < 20)) bad++;
    printf("M68K NATIVE-HEAVY %d seeds, %d mismatches; native %u runs %u links %u\n", n, bad,
           (unsigned)(m68kjit_stats.native - nat0), (unsigned)(m68kjit_stats.block_runs - runs0), (unsigned)m68kjit_stats.links);
    /* one instruction family at a time: each native group gets many runs with
     * edge-value registers and SR saved to RAM after most instructions */
    int fbad = 0;
    for (int f = 0; f < FUZZ_FORMS; f++)
    {
        fuzz_form = f;
        int b = 0;
        for (int i = 0; i < 20; i++)
            if (fuzz_seed(5001 + f * 100 + i, 32, fbad + b < 20)) b++;
        if (b) printf("M68K form %d: %d mismatches\n", f, b);
        fbad += b;
    }
    fuzz_form = -1;
    printf("M68K BY-FORM %d families x 20 seeds, %d mismatches\n", FUZZ_FORMS, fbad);
    /* translating hot blocks only (as the emulators do): same results */
    glue_hot_threshold = 3;
    fuzz_native_bias = 50;
    int hbad2 = 0;
    for (int i = 0; i < 150; i++)
        if (fuzz_seed(9001 + i, 32, hbad2 < 20)) hbad2++;
    printf("M68K HOT-3 150 seeds, %d mismatches\n", hbad2);
    glue_hot_threshold = 0;
    printf("M68K done\n");
}
