/* videobench: the scanline renderer alone, on synthetic frames.
   No ROM: VRAM, OAM, the palette and the display registers are filled from a
   seeded generator, then update_scanline() draws 160 lines exactly as it does
   on the board (the RETRO_GO snapshot path).

   Two jobs:
     - `./videobench bench <scene> [frames]` times one scene and prints the
       pixels drawn, so a change can be weighed against the work it removes;
     - `./videobench fuzz [cases]` hashes the frame of many random display
       configurations, which is the proof that a change draws the same picture
       (compare the hash lines of two builds).

   A scene is a named display setup; `./videobench list` prints them.
*/
#include "common.h"
#include "gba_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hostprof.h"

/* the bits of gpSP the renderer needs from its front end */
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
void gbsp_display_poll(void) {}
#ifdef GBAPROF
u32 gbaprof_pageloads;
extern int64_t gbaprof_render_us;
#endif

static u16 screen[GBA_SCREEN_WIDTH * (GBA_SCREEN_HEIGHT + 1)];

static u32 rnd_state;
static u32 rnd(void)
{
  rnd_state ^= rnd_state << 13;
  rnd_state ^= rnd_state >> 17;
  rnd_state ^= rnd_state << 5;
  return rnd_state;
}

#define IOREG(r, v) io_registers[(r)] = eswap16(v)

/* VRAM holding plausible graphics: tiles of a few shapes (fully opaque rows,
   rows with holes, empty rows) rather than noise, because the renderer's fast
   paths key on exactly that. */
static void fill_vram(int holes_pct)
{
  for (u32 t = 0; t < 1024; t++)
    for (u32 row = 0; row < 8; row++)
    {
      u32 w = rnd();
      if ((int)(rnd() % 100) < holes_pct)
        w &= ~(0xFu << (4 * (rnd() & 7)));   /* a transparent pixel */
      else
        w |= 0x11111111u;                    /* no transparent pixel */
      if ((rnd() & 31) == 0)
        w = 0;                               /* an empty row */
      /* the same words serve the 8bpp and bitmap readings of VRAM */
      ((u32 *)vram)[(t * 8 + row)] = w;
    }
  for (u32 i = 0x8000; i < 0x18000; i += 4)
    *(u32 *)(vram + i) = rnd();
}

static void fill_maps(u32 base_block, u32 ntiles)
{
  u16 *map = (u16 *)&vram[base_block * 2048];
  for (u32 i = 0; i < 2048; i++)
    map[i] = eswap16((u16)((rnd() % ntiles) | ((rnd() & 3) << 10) | ((rnd() & 15) << 12)));
}

static void fill_palette(void)
{
  for (u32 i = 0; i < 512; i++)
    palette_ram_converted[i] = (u16)rnd();
}

static void fill_oam(u32 count, u32 affine_pct, u32 size8bpp_pct)
{
  memset(oam_ram, 0, sizeof(oam_ram));
  for (u32 i = 0; i < count; i++)
  {
    u16 *o = &oam_ram[i * 4];
    u32 y = rnd() % 160, x = rnd() % 240;
    u32 shape = rnd() % 3, size = rnd() % 4;
    u32 a0 = y | (shape << 14) | ((rnd() % 100 < size8bpp_pct) ? 0x2000 : 0);
    if (rnd() % 100 < affine_pct)
      a0 |= 0x100 | ((rnd() & 1) << 9);   /* affine, maybe double size */
    o[0] = eswap16((u16)a0);
    o[1] = eswap16((u16)(x | (size << 14) | ((rnd() & 0x1F) << 9)));
    o[2] = eswap16((u16)((rnd() % 1024) | ((rnd() & 3) << 10) | ((rnd() & 15) << 12)));
  }
  for (u32 i = 0; i < 32; i++)   /* affine parameter sets */
  {
    oam_ram[i * 16 + 3] = eswap16(0x0100 + (rnd() % 0x180) - 0xC0);
    oam_ram[i * 16 + 7] = eswap16((u16)(rnd() % 0x80) - 0x40);
    oam_ram[i * 16 + 11] = eswap16((u16)(rnd() % 0x80) - 0x40);
    oam_ram[i * 16 + 15] = eswap16(0x0100 + (rnd() % 0x180) - 0xC0);
  }
}

typedef struct { const char *name, *what; void (*setup)(void); } scene_t;
static void scene_backdrop(void) { fill_palette(); IOREG(REG_DISPCNT, 0x0000); }

/* Mode 0, four 4bpp text layers, no effects: the common case of most games. */
static void scene_mode0_4bg(void)
{
  fill_vram(10);
  fill_palette();
  for (u32 l = 0; l < 4; l++)
  {
    fill_maps(24 + l, 512);
    IOREG(REG_BG0CNT + l, (u16)(l | ((24 + l) << 8) | (0 << 14)));   /* 256x256, 4bpp */
    IOREG(REG_BG0HOFS + l * 2, (u16)(rnd() % 512));
    IOREG(REG_BG0VOFS + l * 2, (u16)(rnd() % 512));
  }
  IOREG(REG_DISPCNT, 0x0F00);   /* mode 0, BG0-3 on */
}

/* Mode 0, one 4bpp layer: the floor of the per-pixel cost. */
static void scene_mode0_1bg(void)
{
  fill_vram(10);
  fill_palette();
  fill_maps(24, 512);
  IOREG(REG_BG0CNT, (u16)(0 | (24 << 8)));
  IOREG(REG_BG0HOFS, 3);
  IOREG(REG_BG0VOFS, 11);
  IOREG(REG_DISPCNT, 0x0100);
}

/* Mode 0, one 8bpp layer. */
static void scene_mode0_8bpp(void)
{
  fill_vram(10);
  fill_palette();
  fill_maps(24, 512);
  IOREG(REG_BG0CNT, (u16)(0 | (24 << 8) | 0x80));
  IOREG(REG_BG0HOFS, 3);
  IOREG(REG_BG0VOFS, 11);
  IOREG(REG_DISPCNT, 0x0100);
}

/* Two layers blended (the renderer's stacking path). */
static void scene_mode0_blend(void)
{
  scene_mode0_4bg();
  IOREG(REG_BLDCNT, 0x3F41);   /* BG0 1st target, alpha blend, BG1..OBJ 2nd */
  IOREG(REG_BLDALPHA, 0x0808);
}

/* Mode 0 with four layers and 64 sprites: a busy action game. */
static void scene_mode0_objs(void)
{
  scene_mode0_4bg();
  fill_oam(64, 0, 20);
  IOREG(REG_DISPCNT, 0x1F00);   /* + OBJ */
}

/* 64 sprites, half of them rotated or scaled. */
static void scene_objs_affine(void)
{
  scene_mode0_1bg();
  fill_oam(64, 50, 20);
  IOREG(REG_DISPCNT, 0x1100);
}

/* Mode 1: one affine layer (Mario-Kart-like) plus two text layers. */
static void scene_mode1_affine(void)
{
  fill_vram(10);
  fill_palette();
  fill_maps(24, 512);
  fill_maps(25, 512);
  for (u32 i = 0; i < 2048; i++)   /* the affine map is 8bpp tile indices */
    vram[26 * 2048 + i] = (u8)rnd();
  IOREG(REG_BG0CNT, (u16)(0 | (24 << 8)));
  IOREG(REG_BG1CNT, (u16)(1 | (25 << 8)));
  IOREG(REG_BG2CNT, (u16)(2 | (26 << 8) | (2 << 14)));
  IOREG(REG_BG2PA, 0x0100); IOREG(REG_BG2PB, 0x0010);
  IOREG(REG_BG2PC, 0x0020); IOREG(REG_BG2PD, 0x0100);
  IOREG(REG_DISPCNT, 0x0701);   /* mode 1, BG0-2 */
}

/* Mode 3: a 16bpp bitmap. */
static void scene_mode3(void)
{
  fill_vram(0);
  fill_palette();
  IOREG(REG_DISPCNT, 0x0403);
}

/* Mode 4: an 8bpp paletted bitmap. */
static void scene_mode4(void)
{
  fill_vram(0);
  fill_palette();
  IOREG(REG_DISPCNT, 0x0404);
}

/* Four layers inside a window (the per-pixel window path). */
static void scene_window(void)
{
  scene_mode0_objs();
  IOREG(REG_DISPCNT, 0x3F00);        /* + window 0 */
  IOREG(REG_WIN0H, (20 << 8) | 220);
  IOREG(REG_WIN0V, (10 << 8) | 150);
  IOREG(REG_WININ, 0x001F);
  IOREG(REG_WINOUT, 0x0007);
}

static const scene_t scenes[] = {
  {"mode0-1bg",   "mode 0, one 4bpp text layer",            scene_mode0_1bg},
  {"mode0-8bpp",  "mode 0, one 8bpp text layer",            scene_mode0_8bpp},
  {"mode0-4bg",   "mode 0, four 4bpp text layers",          scene_mode0_4bg},
  {"mode0-blend", "mode 0, four layers, alpha blending",    scene_mode0_blend},
  {"mode0-objs",  "mode 0, four layers and 64 sprites",     scene_mode0_objs},
  {"objs-affine", "one layer and 64 sprites, half rotated", scene_objs_affine},
  {"mode1-affine","mode 1, an affine layer and two text",   scene_mode1_affine},
  {"mode3",       "mode 3, a 16bpp bitmap",                 scene_mode3},
  {"mode4",       "mode 4, an 8bpp paletted bitmap",        scene_mode4},
  {"window",      "four layers and sprites inside window 0",scene_window},
  {"backdrop",    "no layer at all: the per-line floor",     scene_backdrop},
};
#define NSCENES ((int)(sizeof(scenes) / sizeof(scenes[0])))

extern u8 gbsp_pal_dirty;

static void draw_frame(int oam_every_line)
{
  for (u32 v = 0; v < 160; v++)
  {
    IOREG(REG_VCOUNT, (u16)v);
    reg[OAM_UPDATED] = (v == 0 || oam_every_line);
    update_scanline();
  }
}

/* the hash is far from free (one serial multiply a pixel): never inside a
   timed loop */
static u32 frame_hash(void)
{
  u32 h = 2166136261u;
  for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++)
    h = (h ^ screen[i]) * 16777619u;
  return h;
}

static int64_t now_us(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void reset_io(void)
{
  memset(io_registers, 0, sizeof(io_registers));
  memset(oam_ram, 0, sizeof(oam_ram));
  affine_reference_x[0] = affine_reference_y[0] = 0;
  affine_reference_x[1] = affine_reference_y[1] = 0;
  gbsp_pal_dirty = 1;
}

int main(int argc, char **argv)
{
  const char *cmd = argc > 1 ? argv[1] : "bench";
  gba_screen_pixels = screen;
  gbsp_memory = calloc(1, sizeof(*gbsp_memory));

  if (!strcmp(cmd, "list"))
  {
    for (int i = 0; i < NSCENES; i++)
      printf("%-14s %s\n", scenes[i].name, scenes[i].what);
    return 0;
  }

  if (!strcmp(cmd, "fuzz"))
  {
    int cases = argc > 2 ? atoi(argv[2]) : 200;
    u32 all = 2166136261u;
    for (int c = 0; c < cases; c++)
    {
      rnd_state = 0x1234567 + c * 7919;
      reset_io();
      /* a random scene, then random display registers on top of it */
      const scene_t *s = &scenes[rnd() % NSCENES];
      s->setup();
      u16 dispcnt = eswap16(io_registers[REG_DISPCNT]);
      dispcnt = (dispcnt & ~0xFF00u) | ((rnd() & 0x1F) << 8) | ((rnd() & 7) << 13);
      IOREG(REG_DISPCNT, dispcnt);
      for (u32 l = 0; l < 4; l++)
      {
        u16 cnt = eswap16(io_registers[REG_BG0CNT + l]);
        IOREG(REG_BG0CNT + l, (u16)(cnt | ((rnd() & 1) << 6) | ((rnd() & 3) << 14)));
        IOREG(REG_BG0HOFS + l * 2, (u16)rnd());
        IOREG(REG_BG0VOFS + l * 2, (u16)rnd());
      }
      IOREG(REG_MOSAIC, (u16)rnd());
      IOREG(REG_BLDCNT, (u16)rnd());
      IOREG(REG_BLDALPHA, (u16)(rnd() & 0x1F1F));
      IOREG(REG_BLDY, (u16)(rnd() & 0x1F));
      IOREG(REG_WIN0H, (u16)rnd()); IOREG(REG_WIN0V, (u16)rnd());
      IOREG(REG_WIN1H, (u16)rnd()); IOREG(REG_WIN1V, (u16)rnd());
      IOREG(REG_WININ, (u16)rnd()); IOREG(REG_WINOUT, (u16)rnd());
      fill_oam(64 + rnd() % 64, rnd() % 100, rnd() % 100);
      draw_frame(rnd() & 1);
      u32 h = frame_hash();
      all = all * 31 + h;
      printf("FUZZ %4d %-14s %08x\n", c, s->name, h);
    }
    printf("FUZZ %d cases hash %08x\n", cases, all);
    return 0;
  }

  /* bench */
  if (getenv("PROF")) prof_start();
  const char *want = argc > 2 ? argv[2] : NULL;
  int frames = argc > 3 ? atoi(argv[3]) : 200;
  int reps = getenv("REPEAT") ? atoi(getenv("REPEAT")) : 5;
  for (int i = 0; i < NSCENES; i++)
  {
    if (want && strcmp(want, scenes[i].name) && strcmp(want, "all"))
      continue;
    rnd_state = 0xC0FFEE;
    reset_io();
    scenes[i].setup();
    int64_t best = 0;
    for (int r = 0; r < reps; r++)
    {
      const int64_t t0 = now_us();
      for (int f = 0; f < frames; f++)
        draw_frame(0);
      const int64_t dt = now_us() - t0;
      if (!best || dt < best)
        best = dt;
    }
    const u32 h = frame_hash();
    printf("BENCH %-14s %8.3f us/frame  hash %08x\n", scenes[i].name, (double)best / frames, h);
  }
  if (getenv("PROF")) prof_dump();
  return 0;
}
