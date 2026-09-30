/* gbajit-test: gpSP on the ESP32-S3 (QEMU or board) with the ROM and a level
 * state taken from flash partitions, the same loop and hashes as the PC
 * harness (esp32-emu-turbo-scratch/gba-work/host.c): FNV over the 240x160
 * RGB565 screen each frame (accumulated as acc = acc * 31 + h) and over the
 * audio samples, printed every 300 frames as "GBAJIT frames N hash H audio A".
 * The input script is the one of the reference runs (right held, B tapped).
 * run.sh builds, puts the ROM and the state into the flash image and runs
 * QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include "esp_vfs.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "common.h"
#include "sound.h"
#include "gba_memory.h"
#include "libretro.h"
#ifdef HAVE_DYNAREC
#include "xjit_exec.h"
int dynarec_enable = 1;
extern u32 xt_exec_delta;
extern u8 *rom_translation_cache, *ram_translation_cache, *rom_translation_ptr, *ram_translation_ptr;
u32 execute_arm_translate(u32 cycles);
#endif

/* what gbsp/main/main.c provides to the core */
u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;
boot_mode selected_boot_mode = boot_game;
u32 skip_next_frame = 0;
int sprite_limit = 1;
gbsp_memory_t *gbsp_memory;
void netpacket_poll_receive() {}
void netpacket_send(uint16_t client_id, const void *buf, size_t len) {}
void set_fastforward_override(bool fastforward) {}
void gbsp_display_poll(void) {}
void gbsp_render_start(void);

/* ---- /rom: the "rom" and "state" partitions as two read-only files ---------- */
static const esp_partition_t *romfs_part[2];
static off_t romfs_pos[2];
static int romfs_open(const char *path, int flags, int mode)
{
    int i = !strcmp(path, "/game.gba") ? 0 : !strcmp(path, "/state") ? 1 : -1;
    if (i < 0 || !romfs_part[i]) { errno = ENOENT; return -1; }
    romfs_pos[i] = 0;
    return i;
}
static ssize_t romfs_read(int fd, void *dst, size_t size)
{
    const esp_partition_t *p = romfs_part[fd];
    if (romfs_pos[fd] + (off_t)size > (off_t)p->size) size = p->size - romfs_pos[fd];
    if (esp_partition_read(p, romfs_pos[fd], dst, size) != ESP_OK) { errno = EIO; return -1; }
    romfs_pos[fd] += size;
    return size;
}
static off_t romfs_lseek(int fd, off_t off, int whence)
{
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? romfs_pos[fd] : (off_t)romfs_part[fd]->size;
    return romfs_pos[fd] = base + off;
}
static int romfs_close(int fd) { return 0; }
static int romfs_fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_size = romfs_part[fd]->size;
    st->st_mode = S_IFREG;
    return 0;
}
static void romfs_register(void)
{
    static const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .open = romfs_open, .read = romfs_read, .lseek = romfs_lseek, .close = romfs_close, .fstat = romfs_fstat,
    };
    romfs_part[0] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "rom");
    romfs_part[1] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "state");
    ESP_ERROR_CHECK(esp_vfs_register("/rom", &vfs, NULL));
}

/* ---- the run ------------------------------------------------------------------ */
static unsigned keys;
static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return keys;
    return (keys >> id) & 1;
}
#ifdef MENU_SCRIPT
/* -DMENU_SCRIPT: from a cold boot (no state), START or A for 8 frames every
   4 seconds, like tapping through a game's menus (compatibility runs) */
static unsigned script(int f)
{
    unsigned phase = f / 240;
    if (f % 240 >= 8)
        return 0;
    return phase % 3 == 0 ? (1 << RETRO_DEVICE_ID_JOYPAD_START) : (1 << RETRO_DEVICE_ID_JOYPAD_A);
}
#else
/* INPUT="0-1200:128,100-110:256,400-410:256,700-710:256" of the reference runs */
static unsigned script(int f)
{
    unsigned m = 0;
    if (f <= 1200) m |= 128;
    if ((f >= 100 && f <= 110) || (f >= 400 && f <= 410) || (f >= 700 && f <= 710)) m |= 256;
    return m;
}
#endif

static void run(void *arg)
{
    static int16_t audio[2 * 2048];
#ifdef MENU_SCRIPT
    const int N = 3600;
#else
    const int N = 600;
#endif
    uint16_t *screen = heap_caps_malloc(GBA_SCREEN_WIDTH * (GBA_SCREEN_HEIGHT + 1) * 2, MALLOC_CAP_SPIRAM);
    printf("GBAJIT start: screen %p, internal free %u KB\n", screen, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    gba_screen_pixels = screen;
    gbsp_memory = heap_caps_calloc(1, sizeof(*gbsp_memory), MALLOC_CAP_SPIRAM);
    libretro_supports_bitmasks = true;
    retro_set_input_state(input_cb);
    gbsp_render_start();
#ifdef HAVE_DYNAREC
    {
        /* translation caches in PSRAM mapped executable, before the ROM cache
           takes the rest */
        static xj_exec_t jit;
        if (!xj_exec_alloc_psram(&jit, ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE))
        {
            printf("GBAJIT no memory for the translation caches\n");
            vTaskDelete(NULL);
        }
        rom_translation_cache = jit.data;
        ram_translation_cache = jit.data + ROM_TRANSLATION_CACHE_SIZE;
        rom_translation_ptr = rom_translation_cache;
        ram_translation_ptr = ram_translation_cache;
        xt_exec_delta = jit.exec - (u32)(uintptr_t)jit.data;
        printf("GBAJIT dynarec: caches %u KB at %p (exec %08lx)\n",
               (unsigned)((ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE) / 1024), jit.data, (unsigned long)jit.exec);
    }
#endif
    init_gamepak_buffer();
    printf("GBAJIT ROM cache ready\n");
    init_sound();
    memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
    if (load_gamepak(NULL, "/rom/game.gba", FEAT_DISABLE, FEAT_DISABLE, SERIAL_MODE_DISABLED) != 0)
    {
        printf("GBAJIT load failed\n");
        vTaskDelete(NULL);
    }
    printf("GBAJIT ROM loaded\n");
    reset_gba();
    {
        u8 *b = heap_caps_malloc(GBA_STATE_MEM_SIZE, MALLOC_CAP_SPIRAM);
        FILE *fp = fopen("/rom/state", "rb");
        size_t got = b && fp ? fread(b, 1, GBA_STATE_MEM_SIZE, fp) : 0;
        int ok = got == GBA_STATE_MEM_SIZE && gba_load_state(b);
        if (!ok && b)   /* which part of the state was refused */
            printf("GBAJIT state checks: cpu %d input %d main %d memory %d sound %d\n", cpu_check_savestate(b), input_check_savestate(b),
                   main_check_savestate(b), memory_check_savestate(b), sound_check_savestate(b));
        if (fp) fclose(fp);
        free(b);
        printf("GBAJIT state loaded: %d (internal free %u KB, PSRAM free %u KB)\n", ok,
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
#ifdef HAVE_DYNAREC
    {
        /* the renderer's VRAM copy (video.cpp), after the state buffer is freed:
           PSRAM is short in QEMU's 8 MB */
        extern void gbsp_rvram_alloc(void);
        gbsp_rvram_alloc();
    }
#endif
    uint32_t acc = 0, ahash = 2166136261u;
    int64_t t0 = esp_timer_get_time();
    for (int f = 0; f < N; f++)
    {
        keys = script(f);
        update_input();
        rumble_frame_reset();
        clear_gamepak_stickybits();
#ifdef HAVE_DYNAREC
        execute_arm_translate(execute_cycles);
#else
        execute_arm(execute_cycles);
#endif
        u32 n = sound_read_samples(audio, 2048);
        for (u32 i = 0; i < n * 2; i++) ahash = (ahash ^ (uint16_t)audio[i]) * 16777619u;
        uint32_t h = 2166136261u;
        for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) h = (h ^ screen[i]) * 16777619u;
        acc = acc * 31 + h;
#if defined(HAVE_DYNAREC) && defined(XTDUMP)
        if (f == 299)   /* host code sample (-DXTDUMP): 4 KB from the middle of the ROM cache, "GBAJITDUMP addr bytes" */
        {
            u32 used = rom_translation_ptr - rom_translation_cache, from = (used / 2) & ~3u;
            printf("GBAJIT code used %u bytes\n", (unsigned)used);
            for (u32 i = 0; i < 4096; i += 32)
            {
                printf("GBAJITDUMP %08x", (unsigned)(xt_exec_delta + (u32)(uintptr_t)rom_translation_cache + from + i));
                for (int k = 0; k < 32; k++) printf(" %02x", rom_translation_cache[from + i + k]);
                printf("\n");
            }
        }
#endif
        if ((f + 1) % 300 == 0)
            printf("GBAJIT frames %d hash %08lx audio %08lx (%.1f ms/frame)\n", f + 1, (unsigned long)acc, (unsigned long)ahash,
                   (esp_timer_get_time() - t0) / 1000.0 / (f + 1));
    }
    printf("GBAJIT done\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    romfs_register();
    if (xTaskCreatePinnedToCore(run, "gbajit", 16384, NULL, 5, NULL, 0) != pdPASS)
        printf("GBAJIT task create failed (internal free %u KB)\n", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}
