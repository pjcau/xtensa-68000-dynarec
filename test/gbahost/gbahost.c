/* gbahost: the board's gpSP sources (RETRO_GO path, interpreter) built and run
   natively, to time one part of a frame against another and to prove that a
   change leaves the picture and the sound untouched.

   The host CPU is not the LX7, so a millisecond here is not a millisecond on
   the board; what carries over is the work counted (lines, tiles, pixels,
   samples, calls) and the share one part takes of the whole.

     ROM=<file> ./gbahost <rom.gba> <frames>
     LOADFILE=<state>   resume from a save state (frame 0 is then in play)
     INPUT=a-b:mask,..  held keys by frame range (libretro joypad bit mask)
     SAVEAT=n SAVEFILE=f  write a save state after frame n
     DUMPAT=n DUMPFILE=f  write frame n as a PPM
     NODRAW=1           emulate without drawing (the CPU side alone)
     TRACE=1            per-frame video and audio hash
     REPEAT=n           run the frame loop n times (warm, for stable timings)
*/
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

#include "hostprof.h"

int dynarec_enable = 0;
u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;
boot_mode selected_boot_mode = boot_game;
u32 skip_next_frame = 0;
int sprite_limit = 1;
gbsp_memory_t *gbsp_memory;
void netpacket_poll_receive(void) {}
void netpacket_send(uint16_t client_id, const void *buf, size_t len) {}
void set_fastforward_override(bool fastforward) {}
/* the app sends a finished frame from inside the scanline code; nothing to do here */
void gbsp_display_poll(void) {}
#ifdef GBAPROF
u32 gbaprof_pageloads;   /* ROM pages the front end faulted in (the board counts them) */
extern int64_t gbaprof_render_us;
#endif

static unsigned keys;
static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return keys;
  return (keys >> id) & 1;
}
/* INPUT=a-b:mask,...  the keys held between frames a and b */
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

static uint16_t screen[GBA_SCREEN_WIDTH * (GBA_SCREEN_HEIGHT + 1)];

static int64_t now_us(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

int main(int c, char **v)
{
  int N = c > 2 ? atoi(v[2]) : 600;
  int repeat = getenv("REPEAT") ? atoi(getenv("REPEAT")) : 1;
  gba_screen_pixels = screen;
  gbsp_memory = calloc(1, sizeof(*gbsp_memory));
  libretro_supports_bitmasks = true;
  retro_set_input_state(input_cb);
  init_gamepak_buffer();
  init_sound();
  memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
  if (load_gamepak(NULL, v[1], FEAT_DISABLE, FEAT_DISABLE, SERIAL_MODE_DISABLED) != 0) { puts("load failed"); return 1; }
  reset_gba();
  u8 *state0 = NULL;
  if (getenv("LOADFILE")) {
    FILE *fp = fopen(getenv("LOADFILE"), "rb");
    if (!fp) { puts("state open failed"); return 1; }
    state0 = malloc(GBA_STATE_MEM_SIZE);
    if (fread(state0, 1, GBA_STATE_MEM_SIZE, fp) != GBA_STATE_MEM_SIZE) { puts("short state"); return 1; }
    fclose(fp);
    if (!gba_load_state(state0)) { puts("state load failed"); return 1; }
  }
  static s16 audio[2 * 2048];
  int dumpat = getenv("DUMPAT") ? atoi(getenv("DUMPAT")) : -1;
  int saveat = getenv("SAVEAT") ? atoi(getenv("SAVEAT")) : -1;
  uint32_t acc = 0, ahash = 0;
  int64_t best_total = 0, best_exec = 0, best_snd = 0, best_rend = 0;

  if (getenv("PROF")) prof_start();
  for (int r = 0; r < repeat; r++) {
    if (r && state0) gba_load_state(state0);
    acc = 0; ahash = 2166136261u;
    int64_t exec_us = 0, snd_us = 0, rend_us = 0;
    const int64_t t_start = now_us();
    for (int f = 0; f < N; f++) {
      keys = script(f);
      skip_next_frame = getenv("NODRAW") ? 1 : 0;
      update_input();
      rumble_frame_reset();
      clear_gamepak_stickybits();
#ifdef GBAPROF
      gbaprof_render_us = 0;
#endif
      const int64_t t0 = now_us();
      execute_arm(execute_cycles);
      const int64_t t1 = now_us();
      u32 n = sound_read_samples(audio, 2048);
      const int64_t t2 = now_us();
      exec_us += t1 - t0;
      snd_us += t2 - t1;
#ifdef GBAPROF
      rend_us += gbaprof_render_us;
#endif
      for (u32 i = 0; i < n * 2; i++) ahash = (ahash ^ (uint16_t)audio[i]) * 16777619u;
      uint32_t h = 2166136261u;
      for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) h = (h ^ screen[i]) * 16777619u;
      acc = acc * 31 + h;
      if (getenv("TRACE")) printf("F %d %08x %08x\n", f, h, ahash);
      if (f + 1 == dumpat && getenv("DUMPFILE")) {
        FILE *o = fopen(getenv("DUMPFILE"), "wb");
        fprintf(o, "P6 %d %d 255\n", GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT);
        for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++) {
          uint16_t p = screen[i];
          fputc((p >> 11) << 3, o); fputc(((p >> 5) & 63) << 2, o); fputc((p & 31) << 3, o);
        }
        fclose(o);
      }
      if (f + 1 == saveat && getenv("SAVEFILE")) {
        u8 *b = malloc(GBA_STATE_MEM_SIZE); gba_save_state(b);
        FILE *o = fopen(getenv("SAVEFILE"), "wb"); fwrite(b, 1, GBA_STATE_MEM_SIZE, o); fclose(o); free(b);
      }
    }
    const int64_t total = now_us() - t_start;
    if (!best_total || total < best_total) {
      best_total = total; best_exec = exec_us; best_snd = snd_us; best_rend = rend_us;
    }
  }
  printf("GBAHOST frames %d hash %08x audio %08x\n", N, acc, ahash);
  printf("GBAHOST ms/frame total %.3f | exec %.3f (render %.3f inside it) sound %.3f\n",
         best_total / 1000.0 / N, best_exec / 1000.0 / N, best_rend / 1000.0 / N, best_snd / 1000.0 / N);
  if (getenv("PROF")) prof_dump();
  return 0;
}
