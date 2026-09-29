/* gameplaySP - Xtensa backend, runtime (esp32-emu-turbo)
 *
 * What x86/x86_stub.S does in assembly, in C: the ARM state, the memory
 * handlers called by translated code (same regions, masks, mirrors and SMC
 * checks as the x86 ones), the update and indirect branch trampolines, CPSR
 * and SPSR writes, SWI, and the entry (xt_enter, generated at init) and exit
 * (retw) of translated code. A handler that must leave the current block
 * returns the instruction address to jump to (0: go on), with the cycles
 * left in reg[XT_CYC_SLOT]. Flags live in reg[REG_N_FLAG..REG_V_FLAG]. */

#include <string.h>
#include <stdio.h>
#include "common.h"
#include "gba_memory.h"
#include "cpu.h"
#include "main.h"
#include "cheats.h"
#include "xjit_emit.h"
#include "xtensa/xtensa_emit.h"

cpu_alert_type function_cc write_io_register8(u32 address, u32 value);
cpu_alert_type function_cc write_io_register16(u32 address, u32 value);
cpu_alert_type function_cc write_io_register32(u32 address, u32 value);
extern u32 rom_cache_watermark;
#define XT_INITIAL_ROM_WATERMARK 16   /* cpu_threaded.c INITIAL_ROM_WATERMARK */

#if defined(ESP_PLATFORM)
#include "esp_cache.h"
#include "esp_attr.h"
#define XT_EXT_BSS EXT_RAM_BSS_ATTR
#else
#define XT_EXT_BSS
#endif

/* ---- ARM state (cpu.cpp defines these only without HAVE_DYNAREC) ---------- */
u32 reg[64];
u32 spsr[6];
u32 reg_mode[7][7];
u16 oam_ram[512];
u16 palette_ram[512];
u16 palette_ram_converted[512];
#ifndef RETRO_GO
XT_EXT_BSS u8 ewram[(1024 * 256) << SMC_DETECTION];
XT_EXT_BSS u8 vram[1024 * 96];
#endif
XT_EXT_BSS u8 iwram[(1024 * 32) << SMC_DETECTION];
u8 *memory_map_read[8 * 1024];
u16 io_registers[512];

xj_emit_t xt_es;

/* the out-of-line emitters behind XT() (xtensa_emit.h) */
u8 *xto_retw(u8 *p)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_retw(&e);
  if (!xj_ok(&e)) xt_emit_error("retw", 0);
  return p + e.pos;
}

u8 *xto_callx8(u8 *p, int a0)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_callx8(&e, a0);
  if (!xj_ok(&e)) xt_emit_error("callx8", 0);
  return p + e.pos;
}

u8 *xto_jx(u8 *p, int a0)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_jx(&e, a0);
  if (!xj_ok(&e)) xt_emit_error("jx", 0);
  return p + e.pos;
}

u8 *xto_ssl(u8 *p, int a0)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_ssl(&e, a0);
  if (!xj_ok(&e)) xt_emit_error("ssl", 0);
  return p + e.pos;
}

u8 *xto_ssr(u8 *p, int a0)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_ssr(&e, a0);
  if (!xj_ok(&e)) xt_emit_error("ssr", 0);
  return p + e.pos;
}

u8 *xto_ssai(u8 *p, int a0)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_ssai(&e, a0);
  if (!xj_ok(&e)) xt_emit_error("ssai", 0);
  return p + e.pos;
}

u8 *xto_j(u8 *p, int a0)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_j(&e, a0);
  if (!xj_ok(&e)) xt_emit_error("j", 0);
  return p + e.pos;
}

u8 *xto_mov(u8 *p, int a0, int a1)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_mov(&e, a0, a1);
  if (!xj_ok(&e)) xt_emit_error("mov", 0);
  return p + e.pos;
}

u8 *xto_movi(u8 *p, int a0, int a1)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_movi(&e, a0, a1);
  if (!xj_ok(&e)) xt_emit_error("movi", 0);
  return p + e.pos;
}

u8 *xto_nsau(u8 *p, int a0, int a1)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_nsau(&e, a0, a1);
  if (!xj_ok(&e)) xt_emit_error("nsau", 0);
  return p + e.pos;
}

u8 *xto_beqz(u8 *p, int a0, int a1)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_beqz(&e, a0, a1);
  if (!xj_ok(&e)) xt_emit_error("beqz", 0);
  return p + e.pos;
}

u8 *xto_bnez(u8 *p, int a0, int a1)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_bnez(&e, a0, a1);
  if (!xj_ok(&e)) xt_emit_error("bnez", 0);
  return p + e.pos;
}

u8 *xto_bgez(u8 *p, int a0, int a1)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_bgez(&e, a0, a1);
  if (!xj_ok(&e)) xt_emit_error("bgez", 0);
  return p + e.pos;
}

u8 *xto_l32r(u8 *p, int a0, int a1)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_l32r(&e, a0, a1);
  if (!xj_ok(&e)) xt_emit_error("l32r", 0);
  return p + e.pos;
}

u8 *xto_entry(u8 *p, int a0, int a1)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_entry(&e, a0, a1);
  if (!xj_ok(&e)) xt_emit_error("entry", 0);
  return p + e.pos;
}

u8 *xto_sll(u8 *p, int a0, int a1)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_sll(&e, a0, a1);
  if (!xj_ok(&e)) xt_emit_error("sll", 0);
  return p + e.pos;
}

u8 *xto_srl(u8 *p, int a0, int a1)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_srl(&e, a0, a1);
  if (!xj_ok(&e)) xt_emit_error("srl", 0);
  return p + e.pos;
}

u8 *xto_sra(u8 *p, int a0, int a1)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_sra(&e, a0, a1);
  if (!xj_ok(&e)) xt_emit_error("sra", 0);
  return p + e.pos;
}

u8 *xto_add(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_add(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("add", 0);
  return p + e.pos;
}

u8 *xto_addi(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_addi(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("addi", 0);
  return p + e.pos;
}

u8 *xto_addmi(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_addmi(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("addmi", 0);
  return p + e.pos;
}

u8 *xto_and(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_and(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("and", 0);
  return p + e.pos;
}

u8 *xto_l32i(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_l32i(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("l32i", 0);
  return p + e.pos;
}

u8 *xto_s32i(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_s32i(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("s32i", 0);
  return p + e.pos;
}

u8 *xto_mull(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_mull(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("mull", 0);
  return p + e.pos;
}

u8 *xto_mulsh(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_mulsh(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("mulsh", 0);
  return p + e.pos;
}

u8 *xto_muluh(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_muluh(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("muluh", 0);
  return p + e.pos;
}

u8 *xto_or(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_or(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("or", 0);
  return p + e.pos;
}

u8 *xto_saltu(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_saltu(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("saltu", 0);
  return p + e.pos;
}

u8 *xto_slli(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_slli(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("slli", 0);
  return p + e.pos;
}

u8 *xto_srai(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_srai(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("srai", 0);
  return p + e.pos;
}

u8 *xto_src(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_src(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("src", 0);
  return p + e.pos;
}

u8 *xto_srli(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_srli(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("srli", 0);
  return p + e.pos;
}

u8 *xto_sub(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_sub(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("sub", 0);
  return p + e.pos;
}

u8 *xto_xor(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_xor(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("xor", 0);
  return p + e.pos;
}

u8 *xto_bltui(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_bltui(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("bltui", 0);
  return p + e.pos;
}

u8 *xto_bgeui(u8 *p, int a0, int a1, int a2)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_bgeui(&e, a0, a1, a2);
  if (!xj_ok(&e)) xt_emit_error("bgeui", 0);
  return p + e.pos;
}

u8 *xto_extui(u8 *p, int a0, int a1, int a2, int a3)
{
  xj_emit_t e;
  xj_init(&e, p, 16);
  xj_extui(&e, a0, a1, a2, a3);
  if (!xj_ok(&e)) xt_emit_error("extui", 0);
  return p + e.pos;
}
u32 xt_exec_delta;
u8 *xt_exit_stub;
static u32 (*xt_enter)(s32 cycles, u32 target, u32 *regs);

void xt_emit_error(const char *what, int line)
{
  printf("xtensa emitter: %s (xtensa emit line %d)\n", what, line);
}

#define XT_EXEC(p) ((u32)(uintptr_t)(p) + xt_exec_delta)

#ifdef RETRO_GO
/* video.cpp's line renderer: VRAM changes wait for the lines already emulated;
   palette changes are picked up per line (gbsp_pal_dirty) */
extern volatile u32 gbsp_rq, gbsp_rd;
extern u8 gbsp_pal_dirty;
void gbsp_render_sync(void);
#define xt_vram_write() do { if (gbsp_rq != gbsp_rd) gbsp_render_sync(); } while (0)
#define xt_pal_write()  (gbsp_pal_dirty = 1)
#else
#define xt_vram_write()
#define xt_pal_write()
#endif

/* ---- flags -------------------------------------------------------------- */
static void collapse_flags(void)
{
  reg[REG_CPSR] = (reg[REG_N_FLAG] << 31) | (reg[REG_Z_FLAG] << 30) | (reg[REG_C_FLAG] << 29) |
                  (reg[REG_V_FLAG] << 28) | (reg[REG_CPSR] & 0xFF);
}
static void extract_flags_regs(void)
{
  reg[REG_N_FLAG] = reg[REG_CPSR] >> 31;
  reg[REG_Z_FLAG] = (reg[REG_CPSR] >> 30) & 1;
  reg[REG_C_FLAG] = (reg[REG_CPSR] >> 29) & 1;
  reg[REG_V_FLAG] = (reg[REG_CPSR] >> 28) & 1;
}

/* ---- jumping back into translated code ---------------------------------- */
static u32 lookup_pc(void)
{
  u8 *p = (reg[REG_CPSR] & 0x20) ? block_lookup_address_thumb(reg[REG_PC])
                                 : block_lookup_address_arm(reg[REG_PC]);
  return XT_EXEC(p);
}
static u32 xt_exit(void) { return XT_EXEC(xt_exit_stub); }

/* x86_update_gba */
static u32 xt_update_gba(u32 pc)
{
  u32 r;
  reg[REG_PC] = pc;
  collapse_flags();
  r = update_gba((s32)reg[XT_CYC_SLOT]);
  if (r & 0x80000000)
    return xt_exit();
  reg[XT_CYC_SLOT] = r & 0x7FFF;
  if (r & 0x40000000)
    return lookup_pc();
  return 0;
}

static u32 xt_indirect_arm(u32 address)   { return XT_EXEC(block_lookup_address_arm(address)); }
static u32 xt_indirect_thumb(u32 address) { return XT_EXEC(block_lookup_address_thumb(address)); }
static u32 xt_indirect_dual(u32 address)  { return XT_EXEC(block_lookup_address_dual(address)); }

/* ---- memory: loads (x86 load_stubs) -------------------------------------- */
#define rd8(p)  (*(const u8 *)(p))
#define rd16(p) ({ u16 _v; memcpy(&_v, (p), 2); _v; })
#define rd32(p) ({ u32 _v; memcpy(&_v, (p), 4); _v; })

/* the region, or 16+ when unaligned for this width (-> the C slow path) */
#define XT_LOAD(name, type, width, almask, slowfn)                           \
static u32 xt_load_##name(u32 address, u32 pc)                               \
{                                                                            \
  u32 r = address >> 24;                                                     \
  if (r > 15 || (address & (almask)))                                        \
    goto slow;                                                               \
  switch (r)                                                                 \
  {                                                                          \
  case 2: return (type)rd##width(&ewram[address & (0x3FFFF & ~(almask))]);   \
  case 3: return (type)rd##width(&iwram[0x8000 + (address & (0x7FFF & ~(almask)))]); \
  case 4: return (type)rd##width((u8 *)io_registers + (address & (0x3FF & ~(almask)))); \
  case 5: return (type)rd##width((u8 *)palette_ram + (address & (0x3FF & ~(almask)))); \
  case 6:                                                                    \
  {                                                                          \
    u32 a = address & (0x1FFFF & ~(almask));                                 \
    if (a >= 0x18000) a -= 0x8000;                                           \
    return (type)rd##width(&vram[a]);                                        \
  }                                                                          \
  case 7: return (type)rd##width((u8 *)oam_ram + (address & (0x3FF & ~(almask)))); \
  case 8: case 9: case 10: case 11: case 12:                                 \
  {                                                                          \
    u8 *map = memory_map_read[address >> 15];                                \
    if (!map) goto slow;                                                     \
    return (type)rd##width(map + (address & 0x7FFF));                        \
  }                                                                          \
  }                                                                          \
slow:                                                                        \
  reg[REG_PC] = pc;                                                          \
  return slowfn(address);                                                    \
}
XT_LOAD(u32, u32, 32, 3, read_memory32)
XT_LOAD(u16, u16, 16, 1, read_memory16)
XT_LOAD(s16, s16, 16, 1, read_memory16s)
XT_LOAD(u8, u8, 8, 0, read_memory8)
XT_LOAD(s8, s8, 8, 0, read_memory8s)

/* ---- memory: stores (x86 write_stubs) ------------------------------------ */
/* write_epilogue: act on the alerts of an I/O write, then go on at reg[PC] */
static u32 xt_write_epilogue(u32 alert)
{
  collapse_flags();
  if (alert & CPU_ALERT_SMC)
    flush_translation_cache_ram();
  if (alert & CPU_ALERT_IRQ)
    check_and_raise_interrupts();
  if (alert & CPU_ALERT_HALT)
  {
    u32 r = update_gba((s32)reg[XT_CYC_SLOT]);
    if (r & 0x80000000)
      return xt_exit();
    reg[XT_CYC_SLOT] = r & 0x7FFF;
  }
  return lookup_pc();
}
static u32 xt_smc_write(void)
{
  flush_translation_cache_ram();
  return lookup_pc();
}

/* palette: the x86 stub's conversion (bit 15 lands in green's low bit) */
static void xt_store_palette16b(u32 a, u32 v)
{
  u32 c;
  v &= 0xFFFF;
  memcpy((u8 *)palette_ram + a, &(u16){v}, 2);
  c = ((v << 11) & 0xF800) | ((v & 0x03E0) << 1) | ((v >> 10) & 0x3F);
  memcpy((u8 *)palette_ram_converted + a, &(u16){c}, 2);
  xt_pal_write();
}
static void xt_store_vram16(u32 a, u32 v)
{
  if (a >= 0x18000) a -= 0x8000;
  xt_vram_write();
  memcpy(&vram[a], &(u16){v}, 2);
}

static u32 xt_store_u32(u32 address, u32 value)
{
  u32 a;
  switch (address >> 24)
  {
  case 2:
    a = address & 0x3FFFC;
    memcpy(&ewram[a], &value, 4);
    if (rd32(&ewram[a + 0x40000])) return xt_smc_write();
    return 0;
  case 3:
    a = address & 0x7FFC;
    memcpy(&iwram[0x8000 + a], &value, 4);
    if (rd32(&iwram[a])) return xt_smc_write();
    return 0;
  case 4:
  {
    cpu_alert_type al = write_io_register32(address & 0x3FC, value);
    return al ? xt_write_epilogue(al) : 0;
  }
  case 5:
    a = address & 0x3FF;
    xt_store_palette16b(a, value);
    xt_store_palette16b(a + 2, value >> 16);
    return 0;
  case 6:
    a = address & 0x1FFFC;
    if (a >= 0x18000) a -= 0x8000;
    xt_vram_write();
    memcpy(&vram[a], &value, 4);
    return 0;
  case 7:
    reg[OAM_UPDATED] = 1;
    memcpy((u8 *)oam_ram + (address & 0x3FC), &value, 4);
    return 0;
  }
  return 0;   /* BIOS, gamepak, EEPROM/backup (32-bit): ignored */
}

static u32 xt_store_aligned_u32(u32 address, u32 value)
{
  u32 a;
  switch (address >> 24)
  {
  case 2: a = address & 0x3FFFC; memcpy(&ewram[a], &value, 4); return 0;   /* no SMC check */
  case 3: a = address & 0x7FFC; memcpy(&iwram[0x8000 + a], &value, 4); return 0;
  case 4: case 5: case 6: case 7:
    return xt_store_u32(address, value);
  }
  return 0;
}

static u32 xt_store_u16(u32 address, u32 value)
{
  u32 a;
  switch (address >> 24)
  {
  case 2:
    a = address & 0x3FFFE;
    memcpy(&ewram[a], &(u16){value}, 2);
    if (rd16(&ewram[a + 0x40000])) return xt_smc_write();
    return 0;
  case 3:
    a = address & 0x7FFE;
    memcpy(&iwram[0x8000 + a], &(u16){value}, 2);
    if (rd16(&iwram[a])) return xt_smc_write();
    return 0;
  case 4:
  {
    cpu_alert_type al = write_io_register16(address & 0x3FE, value);
    return al ? xt_write_epilogue(al) : 0;
  }
  case 5: xt_store_palette16b(address & 0x3FF, value); return 0;
  case 6: xt_store_vram16(address & 0x1FFFE, value); return 0;
  case 7:
    reg[OAM_UPDATED] = 1;
    memcpy((u8 *)oam_ram + (address & 0x3FE), &(u16){value}, 2);
    return 0;
  case 8: write_gpio(address & 0xFF, value & 0xFFFF); return 0;
  case 13: write_eeprom(address, value); return 0;
  }
  return 0;
}

static u32 xt_store_u8(u32 address, u32 value)
{
  u32 a, d = (value & 0xFF) | ((value & 0xFF) << 8);   /* 16-bit bus: the byte twice */
  switch (address >> 24)
  {
  case 2:
    a = address & 0x3FFFF;
    ewram[a] = value;
    if (ewram[a + 0x40000]) return xt_smc_write();
    return 0;
  case 3:
    a = address & 0x7FFF;
    iwram[0x8000 + a] = value;
    if (iwram[a]) return xt_smc_write();
    return 0;
  case 4:
  {
    cpu_alert_type al = write_io_register8(address & 0x3FF, value);
    return al ? xt_write_epilogue(al) : 0;
  }
  case 5: xt_store_palette16b(address & 0x3FE, d); return 0;
  case 6: xt_store_vram16(address & 0x1FFFE, d); return 0;
  case 7:
    reg[OAM_UPDATED] = 1;
    memcpy((u8 *)oam_ram + (address & 0x3FE), &(u16){d}, 2);
    return 0;
  case 14: write_backup(address & 0xFFFF, value & 0xFF); return 0;
  }
  return 0;
}

/* ---- CPSR / SPSR / SWI ------------------------------------------------------- */
static u32 xt_read_cpsr(void)
{
  collapse_flags();
  return reg[REG_CPSR];
}
static u32 xt_read_spsr(void)
{
  collapse_flags();
  return spsr[reg[CPU_MODE] & 0xF];
}

u32 execute_store_cpsr_body(void)
{
  set_cpu_mode(cpu_modes[reg[REG_CPSR] & 0xF]);
  if((io_registers[REG_IE] & io_registers[REG_IF]) &&
      io_registers[REG_IME] && ((reg[REG_CPSR] & 0x80) == 0))
  {
    REG_MODE(MODE_IRQ)[6] = reg[REG_PC] + 4;
    REG_SPSR(MODE_IRQ) = reg[REG_CPSR];
    reg[REG_CPSR] = (reg[REG_CPSR] & 0xFFFFFF00) | 0xD2;
    set_cpu_mode(MODE_IRQ);
    return 0x00000018;
  }
  return 0;
}

static u32 xt_store_cpsr(u32 new_cpsr, u32 mask_user, u32 mask_sys)
{
  u32 mask = (reg[CPU_MODE] & 0x10) ? mask_sys : mask_user, r;
  reg[REG_CPSR] = (new_cpsr & mask) | (reg[REG_CPSR] & ~mask);
  if (!(mask & 0xFF))
  {
    extract_flags_regs();
    return 0;
  }
  r = execute_store_cpsr_body();
  extract_flags_regs();
  return r ? XT_EXEC(block_lookup_address_arm(r)) : 0;
}

static void xt_store_spsr(u32 value, u32 mask)
{
  u32 *s = &spsr[reg[CPU_MODE] & 0xF];
  *s = (value & mask) | (*s & ~mask);
}

u32 execute_spsr_restore(u32 address)
{
  if(reg[CPU_MODE] != MODE_USER && reg[CPU_MODE] != MODE_SYSTEM)
  {
    reg[REG_CPSR] = REG_SPSR(reg[CPU_MODE]);
    extract_flags_regs();
    set_cpu_mode(cpu_modes[reg[REG_CPSR] & 0xF]);

    if((io_registers[REG_IE] & io_registers[REG_IF]) &&
     io_registers[REG_IME] && ((reg[REG_CPSR] & 0x80) == 0))
    {
      REG_MODE(MODE_IRQ)[6] = reg[REG_PC] + 4;
      REG_SPSR(MODE_IRQ) = reg[REG_CPSR];
      reg[REG_CPSR] = 0xD2;
      address = 0x00000018;
      set_cpu_mode(MODE_IRQ);
    }

    if(reg[REG_CPSR] & 0x20)
      address |= 0x01;
  }
  return address;
}

static void xt_swi(u32 pc)
{
  collapse_flags();
  reg[REG_BUS_VALUE] = 0xe3a02004;
  REG_MODE(MODE_SUPERVISOR)[6] = pc;
  REG_SPSR(MODE_SUPERVISOR) = reg[REG_CPSR];
  reg[REG_CPSR] = (reg[REG_CPSR] & ~0x3F) | 0x13 | 0x80;
  set_cpu_mode(MODE_SUPERVISOR);
}

/* the x86 inline division (skipped on a zero divisor) */
static void xt_hle_div_common(s32 num, s32 den)
{
  s32 q, r;
  if (!den)
    return;
  q = num / den;
  r = num % den;
  reg[0] = q;
  reg[1] = r;
  reg[3] = q < 0 ? -q : q;
}
static void xt_hle_div(void)     { xt_hle_div_common(reg[0], reg[1]); }
static void xt_hle_div_arm(void) { xt_hle_div_common(reg[1], reg[0]); }

/* ---- caches ------------------------------------------------------------------ */
void platform_cache_sync(void *baseaddr, void *endptr)
{
#if defined(ESP_PLATFORM)
  uintptr_t a = (uintptr_t)baseaddr & ~(uintptr_t)63, b = ((uintptr_t)endptr + 63) & ~(uintptr_t)63;
  if (b <= a)
    return;
  esp_cache_msync((void *)a, b - a, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  esp_cache_msync((void *)(a + xt_exec_delta), b - a, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
  __asm__ volatile("memw; isync");
#endif
}

/* ---- init and entry ------------------------------------------------------------ */
void init_emitter(bool must_swap)
{
  u8 *translation_ptr = &rom_translation_cache[XT_INITIAL_ROM_WATERMARK];
  static void *const fn[XT_FN_COUNT] = {
    [XT_FN_UPDATE_GBA] = xt_update_gba,
    [XT_FN_INDIRECT_ARM] = xt_indirect_arm,
    [XT_FN_INDIRECT_THUMB] = xt_indirect_thumb,
    [XT_FN_INDIRECT_DUAL] = xt_indirect_dual,
    [XT_FN_LOAD_U8] = xt_load_u8,
    [XT_FN_LOAD_S8] = xt_load_s8,
    [XT_FN_LOAD_U16] = xt_load_u16,
    [XT_FN_LOAD_S16] = xt_load_s16,
    [XT_FN_LOAD_U32] = xt_load_u32,
    [XT_FN_STORE_U8] = xt_store_u8,
    [XT_FN_STORE_U16] = xt_store_u16,
    [XT_FN_STORE_U32] = xt_store_u32,
    [XT_FN_STORE_ALIGNED_U32] = xt_store_aligned_u32,
    [XT_FN_READ_CPSR] = xt_read_cpsr,
    [XT_FN_READ_SPSR] = xt_read_spsr,
    [XT_FN_STORE_CPSR] = xt_store_cpsr,
    [XT_FN_STORE_SPSR] = xt_store_spsr,
    [XT_FN_SPSR_RESTORE] = execute_spsr_restore,
    [XT_FN_SWI] = xt_swi,
    [XT_FN_PROCESS_CHEATS] = process_cheats,
    [XT_FN_HLE_DIV] = xt_hle_div,
    [XT_FN_HLE_DIV_ARM] = xt_hle_div_arm,
  };
  for (int i = 0; i < XT_FN_COUNT; i++)
    reg[XT_FN_SLOT + i] = (u32)(uintptr_t)fn[i];

  /* xt_enter(cycles, target, regs): entry a1,32; the registers of
     translated code; jx target */
  translation_ptr = (u8 *)(((uintptr_t)translation_ptr + 3) & ~(uintptr_t)3);
  xt_enter = (void *)(uintptr_t)XT_EXEC(translation_ptr);
  XT(entry, 1, 32);
  XT(mov, 9, 3);            /* target */
  XT(mov, reg_cycles, 2);   /* a3 = cycles */
  XT(mov, reg_base, 4);     /* a2 = &reg[0] */
  XT(jx, 9);
  /* leaving translated code: back to xt_enter's caller */
  xt_exit_stub = translation_ptr;
  XT(retw);
  translation_ptr = (u8 *)(((uintptr_t)translation_ptr + 15) & ~(uintptr_t)15);
  platform_cache_sync(rom_translation_cache, translation_ptr);
  rom_cache_watermark = (u32)(translation_ptr - rom_translation_cache);
  init_bios_hooks();
}

u32 execute_arm_translate(u32 cycles)
{
  u32 target;
  extract_flags_regs();
  reg[XT_CYC_SLOT] = cycles;
  if (reg[CPU_HALT_STATE])
  {
    /* cpu_sleep_loop */
    u32 r = update_gba((s32)cycles);
    if (r & 0x80000000)
      return 0;
    reg[XT_CYC_SLOT] = r & 0x7FFF;
  }
  target = lookup_pc();
  xt_enter((s32)reg[XT_CYC_SLOT], target, reg);
  return 0;
}
