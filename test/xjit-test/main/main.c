/* xjit-test, step 0: can the ESP32-S3 run code written at run time?
 *
 * 1. a C function through a pointer (the control)
 * 2. machine code written into internal RAM (heap, MALLOC_CAP_EXEC) and called
 * 3. the same code written into PSRAM, mapped for instruction fetch with
 *    esp_mmu_map(MMU_MEM_CAP_EXEC) and called, after writing the data cache
 *    back and invalidating the instruction cache
 * Cycle counts are printed too; they only mean something on the board (QEMU
 * does not model caches or PSRAM timing). Results are "XJIT ..." lines. */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_mmu_map.h"
#include "esp_cache.h"
#include "esp_chip_info.h"

/* xtensa-esp32s3-elf-as output (windowed ABI):
 *   f42:  entry a1,32 ; movi.n a2,42 ; retw.n
 *   fadd: entry a1,32 ; add.n a2,a2,a3 ; retw.n */
static const uint8_t code_f42[] = {0x36, 0x41, 0x00, 0x2c, 0xa2, 0x1d, 0xf0, 0x00};
static const uint8_t code_fadd[] = {0x36, 0x41, 0x00, 0x3a, 0x22, 0x1d, 0xf0, 0x00};

typedef int (*fn0_t)(void);
typedef int (*fn2_t)(int, int);

static int c_f42(void) { return 42; }

/* internal RAM may only be written 32 bits at a time through its I-bus alias */
static void copy_words(void *dst, const void *src, size_t len)
{
    volatile uint32_t *d = dst;
    for (size_t i = 0; i < len; i += 4)
    {
        uint32_t w;
        memcpy(&w, (const uint8_t *)src + i, 4);
        d[i / 4] = w;
    }
}

static uint32_t cycles_per_call(fn0_t f)
{
    const int n = 100000;
    uint32_t t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < n; i++)
        f();
    return (esp_cpu_get_cycle_count() - t0) * 10 / n;   /* tenths of a cycle */
}

static void report(const char *what, fn0_t f0, fn2_t f2)
{
    int r0 = f0(), r2 = f2 ? f2(40, 2) : 42;
    uint32_t c = cycles_per_call(f0);
    printf("XJIT %-8s f42=%d fadd(40,2)=%d %s | %" PRIu32 ".%" PRIu32 " cycles/call (board only)\n",
           what, r0, r2, (r0 == 42 && r2 == 42) ? "PASS" : "FAIL", c / 10, c % 10);
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("XJIT start: model %d rev %d, cores %d, internal free %u KB, PSRAM free %u KB\n",
           chip.model, chip.revision, chip.cores,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* 1. control */
    report("C", c_f42, NULL);

    /* 2. internal RAM */
    uint8_t *iram = heap_caps_aligned_alloc(4, 64, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    printf("XJIT iram buffer %p\n", iram);
    if (iram)
    {
        copy_words(iram, code_f42, sizeof(code_f42));
        copy_words(iram + 16, code_fadd, sizeof(code_fadd));
        __asm__ volatile("memw; isync");
        report("IRAM", (fn0_t)iram, (fn2_t)(iram + 16));
    }
    else
        printf("XJIT IRAM FAIL: no MALLOC_CAP_EXEC memory\n");

    /* 3. PSRAM mapped for instruction fetch */
    const size_t page = 0x10000;   /* MMU page on the S3 */
    uint8_t *psram = heap_caps_aligned_alloc(page, page, MALLOC_CAP_SPIRAM);
    printf("XJIT psram buffer %p\n", psram);
    if (psram)
    {
        memcpy(psram, code_f42, sizeof(code_f42));
        memcpy(psram + 16, code_fadd, sizeof(code_fadd));
        esp_err_t e1 = esp_cache_msync(psram, page, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        esp_paddr_t paddr = 0;
        mmu_target_t target = 0;
        esp_err_t e2 = esp_mmu_vaddr_to_paddr(psram, &paddr, &target);
        void *exec = NULL;
        esp_err_t e3 = e2 == ESP_OK ? esp_mmu_map(paddr, page, target, MMU_MEM_CAP_EXEC, 0, &exec) : e2;
        printf("XJIT psram msync %d, paddr 0x%" PRIx32 " target %d (%d), map %d -> %p\n",
               e1, (uint32_t)paddr, (int)target, e2, e3, exec);
        if (e3 == ESP_OK && exec)
        {
            esp_err_t e4 = esp_cache_msync(exec, page, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
            printf("XJIT psram icache invalidate %d\n", e4);
            report("PSRAM", (fn0_t)exec, (fn2_t)((uint8_t *)exec + 16));
        }
        else
            printf("XJIT PSRAM FAIL: no executable mapping\n");
    }
    printf("XJIT done\n");
}
