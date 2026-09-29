/* thumb_gpsp_check.c - the step-4 Thumb reference (main/thumb.c) against gpSP's
 * own interpreter, on the PC. Each random sequence is written to IWRAM at
 * 0x03000100 followed by "b ." and run by execute_arm() (interrupts off) from
 * the same random register/flag state; registers and N/Z/C/V are compared.
 *   run_thumb_gpsp_check.sh <any .gba ROM> [sequences]   (see that script) */
#include "common.h"
#include "gba_memory.h"
#include "sound.h"
#include "libretro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "thumb.h"

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;
boot_mode selected_boot_mode = boot_game;
u32 skip_next_frame = 1;
int sprite_limit = 1;
gbsp_memory_t *gbsp_memory;
void netpacket_poll_receive() {}
void netpacket_send(uint16_t client_id, const void *buf, size_t len) {}
void set_fastforward_override(bool fastforward) {}
static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id) { return 0; }
static uint16_t screen[GBA_SCREEN_WIDTH * (GBA_SCREEN_HEIGHT + 1)];

int main(int argc, char **argv)
{
  int N = argc > 2 ? atoi(argv[2]) : 3000, fails = 0, runs = 0;
  gba_screen_pixels = screen;
  gbsp_memory = calloc(1, sizeof(*gbsp_memory));
  libretro_supports_bitmasks = true;
  retro_set_input_state(input_cb);
  init_gamepak_buffer();
  init_sound();
  memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
  if (load_gamepak(NULL, argv[1], FEAT_DISABLE, FEAT_DISABLE, SERIAL_MODE_DISABLED) != 0) { puts("load failed"); return 1; }
  reset_gba();

  for (int n = 0; n < N; n++)
  {
    uint16_t code[32];
    int count = 1 + rand() % 24;
    for (int i = 0; i < count; i++) code[i] = thumb_random_op();
    for (int v = 0; v < 6; v++)
    {
      thumb_state_t s0, want;
      thumb_random_state(&s0);
      want = s0;
      thumb_ref(&want, code, count);

      for (int i = 0; i < count; i++) { iwram[0x100 + 2 * i] = code[i] & 0xFF; iwram[0x101 + 2 * i] = code[i] >> 8; }
      iwram[0x100 + 2 * count] = 0xFE; iwram[0x101 + 2 * count] = 0xE7;   /* b . */
      for (int i = 0; i < 15; i++) reg[i] = s0.r[i];
      reg[REG_PC] = 0x03000100;
      reg[REG_CPSR] = (s0.n << 31) | (s0.z << 30) | (s0.c << 29) | (s0.v << 28) | 0x80 | 0x40 | 0x20 | 0x1F;   /* I, F, T, system */
      reg[CPU_HALT_STATE] = CPU_ACTIVE;
      write_ioreg(REG_IME, 0);
      execute_arm(execute_cycles);
      runs++;

      thumb_state_t got = s0;
      for (int i = 0; i < 15; i++) got.r[i] = reg[i];
      got.n = reg[REG_CPSR] >> 31; got.z = (reg[REG_CPSR] >> 30) & 1; got.c = (reg[REG_CPSR] >> 29) & 1; got.v = (reg[REG_CPSR] >> 28) & 1;
      got.r[15] = want.r[15];
      if (reg[REG_PC] != 0x03000100u + 2 * count || memcmp(&got, &want, sizeof(got)))
      {
        if (fails++ < 6)
        {
          printf("DIFF seq %d (%d ops) pc %08x:", n, count, reg[REG_PC]);
          for (int i = 0; i < count && i < 6; i++) printf(" %04x", code[i]);
          printf("\n");
          for (int k = 0; k < 20; k++)
            if (((uint32_t *)&got)[k] != ((uint32_t *)&want)[k])
              printf("  word %d: gpSP %08x reference %08x\n", k, ((uint32_t *)&got)[k], ((uint32_t *)&want)[k]);
        }
        break;
      }
    }
  }
  printf("thumb reference vs gpSP: %d runs, %d failing sequences\n", runs, fails);
  return fails != 0;
}
