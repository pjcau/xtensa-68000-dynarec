/* PC harness for retro-go's gbsp (gpSP interpreter): the loop of gbsp/main/main.c
   without retro-go. Frame hash (FNV over the 240x160 RGB565 screen), audio hash.
   ./gbahost rom.gba [frames]
   env: INPUT="from-to:mask,..." (libretro joypad bits: B0 Y1 SEL2 START3 UP4 DOWN5 LEFT6 RIGHT7 A8 X9 L10 R11)
        DUMPAT=n DUMPFILE=x.ppm  SAVEAT=n SAVEFILE=f  LOADFILE=f  NODRAW=1 (skip every frame's render) */
#include "common.h"
#include "memmap.h"
#include "sound.h"
#include "gba_memory.h"
#include "gba_cc_lut.h"
#include "libretro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

int dynarec_enable = 1;
u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;
boot_mode selected_boot_mode = boot_game;
u32 skip_next_frame = 0;
int sprite_limit = 1;
#ifdef RETRO_GO
gbsp_memory_t *gbsp_memory;
#endif
void netpacket_poll_receive() {}
void netpacket_send(uint16_t client_id, const void *buf, size_t len) {}
void set_fastforward_override(bool fastforward) {}

static unsigned keys;
static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return keys;
  return (keys >> id) & 1;
}
static unsigned script(int f)
{
  const char *s = getenv("INPUT");
  unsigned m = 0;
  while (s && *s) {
    int a, b; unsigned k; int n = 0;
    if (sscanf(s, "%d-%d:%u%n", &a, &b, &k, &n) != 3) break;
    if (f >= a && f <= b) m |= k;
    s += n; if (*s == ',') s++;
  }
  return m;
}
extern void pchist_dump(int top) __attribute__((weak));
extern int m4a_disable __attribute__((weak));
static uint16_t screen[GBA_SCREEN_WIDTH * (GBA_SCREEN_HEIGHT + 1)];

int main(int c, char **v)
{
  int N = c > 2 ? atoi(v[2]) : 600;
  gba_screen_pixels = screen;
#ifdef RETRO_GO
  gbsp_memory = calloc(1, sizeof(*gbsp_memory));
#endif
  libretro_supports_bitmasks = true;
  retro_set_input_state(input_cb);
  init_gamepak_buffer();
  init_sound();
  memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
  if (load_gamepak(NULL, v[1], FEAT_DISABLE, FEAT_DISABLE, SERIAL_MODE_DISABLED) != 0) { puts("load failed"); return 1; }
#ifdef MMAP_JIT_CACHE
  rom_translation_cache = mmap(NULL, ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ram_translation_cache = rom_translation_cache + ROM_TRANSLATION_CACHE_SIZE;
  rom_translation_ptr = rom_translation_cache;
  ram_translation_ptr = ram_translation_cache;
#endif
  reset_gba();
  if (&m4a_disable && getenv("M4AOFF")) m4a_disable = 1;
  if (getenv("NOJIT")) dynarec_enable = 0;
  if (getenv("IDLEPC")) idle_loop_target_pc = strtoul(getenv("IDLEPC"), 0, 16);
  if (getenv("LOADFILE")) {
    FILE *fp = fopen(getenv("LOADFILE"), "rb"); u8 *b = malloc(GBA_STATE_MEM_SIZE);
    fread(b, 1, GBA_STATE_MEM_SIZE, fp); fclose(fp); printf("#load %d\n", gba_load_state(b)); free(b);
  }
  static s16 audio[2 * 2048];
  uint32_t acc = 0, ahash = 2166136261u;
  int dumpat = getenv("DUMPAT") ? atoi(getenv("DUMPAT")) : -1, saveat = getenv("SAVEAT") ? atoi(getenv("SAVEAT")) : -1;
  struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int f = 0; f < N; f++) {
    keys = script(f);
    skip_next_frame = getenv("NODRAW") ? 1 : 0;
    update_input();
    rumble_frame_reset();
    clear_gamepak_stickybits();
    if (dynarec_enable) execute_arm_translate(execute_cycles); else execute_arm(execute_cycles);
    u32 n = sound_read_samples(audio, 2048);
    for (u32 i = 0; i < n * 2; i++) ahash = (ahash ^ (uint16_t)audio[i]) * 16777619u;
    uint32_t h = 2166136261u;
    for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) h = (h ^ screen[i]) * 16777619u;
    acc = acc * 31 + h;
    if (getenv("TRACE")) printf("F %d %08x %08x\n", f, h, ahash);
    if (f + 1 == dumpat) {
      FILE *o = fopen(getenv("DUMPFILE"), "wb"); fprintf(o, "P6 %d %d 255\n", GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT);
      for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) { uint16_t p = screen[i]; fputc((p >> 11) << 3, o); fputc(((p >> 5) & 63) << 2, o); fputc((p & 31) << 3, o); }
      fclose(o);
    }
    if (f + 1 == saveat) {
      u8 *b = malloc(GBA_STATE_MEM_SIZE); gba_save_state(b);
      FILE *o = fopen(getenv("SAVEFILE"), "wb"); fwrite(b, 1, GBA_STATE_MEM_SIZE, o); fclose(o); free(b);
    }
    if ((f + 1) % 300 == 0) printf("frames %d hash %08x audio %08x\n", f + 1, acc, ahash);
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  if (getenv("IWRAMDUMP")) { FILE *o = fopen(getenv("IWRAMDUMP"), "wb"); fwrite(iwram, 1, 0x8000, o); fclose(o); }
  if (pchist_dump && getenv("PCHIST")) pchist_dump(atoi(getenv("PCHIST")));
  printf("time %.3f s for %d frames (%.2f ms/frame)\n", (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9, N,
         ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / N);
  return 0;
}
