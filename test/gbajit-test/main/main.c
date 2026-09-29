/* gbajit-test: gpSP on the ESP32-S3 (QEMU or board) with the ROM and a level
 * state taken from flash partitions, the same loop and hashes as the PC
 * harness (esp32-emu-turbo-scratch/gba-work/host.c): FNV over the 240x160
 * RGB565 screen each frame (accumulated as acc = acc * 31 + h) and over the
 * audio samples, printed every 300 frames as "GBAJIT frames N hash H audio A".
 * The input script is the one of the reference runs (right held, B tapped).
 * run.sh builds, puts the ROM and the state into the flash image and runs
 * QEMU. */
#include <stdio.h>
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
/* INPUT="0-1200:128,100-110:256,400-410:256,700-710:256" of the reference runs */
static unsigned script(int f)
{
    unsigned m = 0;
    if (f <= 1200) m |= 128;
    if ((f >= 100 && f <= 110) || (f >= 400 && f <= 410) || (f >= 700 && f <= 710)) m |= 256;
    return m;
}

static void run(void *arg)
{
    static int16_t audio[2 * 2048];
    const int N = 600;
    uint16_t *screen = heap_caps_malloc(GBA_SCREEN_WIDTH * (GBA_SCREEN_HEIGHT + 1) * 2, MALLOC_CAP_SPIRAM);
    printf("GBAJIT start: screen %p, internal free %u KB\n", screen, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    gba_screen_pixels = screen;
    gbsp_memory = heap_caps_calloc(1, sizeof(*gbsp_memory), MALLOC_CAP_SPIRAM);
    libretro_supports_bitmasks = true;
    retro_set_input_state(input_cb);
    gbsp_render_start();
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
        if (b)
        {
            extern uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);
            uint8_t t16[16] __attribute__((aligned(4)));
            esp_partition_read(romfs_part[1], 0, t16, 16);
            printf("GBAJIT direct read @0 (internal): %02x %02x %02x %02x %02x %02x\n", t16[0], t16[1], t16[2], t16[3], t16[4], t16[5]);
            uint8_t *ps = heap_caps_malloc(64, MALLOC_CAP_SPIRAM);
            esp_partition_read(romfs_part[1], 0, ps, 16);
            printf("GBAJIT direct read @0 (PSRAM %p): %02x %02x %02x %02x %02x %02x\n", ps, ps[0], ps[1], ps[2], ps[3], ps[4], ps[5]);
            {
                static const uint32_t fa[] = {0x400000, 0x7FFFFC, 0x800000, 0x900000, 0xBFFFF0, 0xC00000, 0xC00010};
                for (int i = 0; i < 7; i++)
                {
                    uint8_t x[8] __attribute__((aligned(4)));
                    esp_flash_read(NULL, x, fa[i], 8);
                    printf("GBAJIT flash @%06lx: %02x %02x %02x %02x %02x %02x %02x %02x\n", (unsigned long)fa[i], x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
                }
            }
            static const int offs[] = {0, 16, 4096, 65536, 65536 + 4096, 200000, 425984 - 16};
            for (int i = 0; i < 7; i++)
                printf("GBAJIT state @%d: %02x %02x %02x %02x\n", offs[i], b[offs[i]], b[offs[i] + 1], b[offs[i] + 2], b[offs[i] + 3]);
            printf("GBAJIT state crc %08lx (first 64 KB %08lx)\n", (unsigned long)esp_rom_crc32_le(0, b, GBA_STATE_MEM_SIZE), (unsigned long)esp_rom_crc32_le(0, b, 65536));
        }
        if (!ok && b)
            printf("GBAJIT checks: cpu %d input %d main %d memory %d sound %d\n", cpu_check_savestate(b), input_check_savestate(b),
                   main_check_savestate(b), memory_check_savestate(b), sound_check_savestate(b));
        printf("GBAJIT state: buffer %p, file %p, read %u of %u, magic %02x%02x%02x%02x\n", b, fp, (unsigned)got,
               (unsigned)GBA_STATE_MEM_SIZE, b ? b[0] : 0, b ? b[1] : 0, b ? b[2] : 0, b ? b[3] : 0);
        if (fp) fclose(fp);
        free(b);
        printf("GBAJIT state loaded: %d (internal free %u KB, PSRAM free %u KB)\n", ok,
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
    uint32_t acc = 0, ahash = 2166136261u;
    int64_t t0 = esp_timer_get_time();
    for (int f = 0; f < N; f++)
    {
        keys = script(f);
        update_input();
        rumble_frame_reset();
        clear_gamepak_stickybits();
        execute_arm(execute_cycles);
        u32 n = sound_read_samples(audio, 2048);
        for (u32 i = 0; i < n * 2; i++) ahash = (ahash ^ (uint16_t)audio[i]) * 16777619u;
        uint32_t h = 2166136261u;
        for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) h = (h ^ screen[i]) * 16777619u;
        acc = acc * 31 + h;
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
