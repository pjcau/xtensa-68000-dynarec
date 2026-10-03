# 68000 roadmap

The 68000 frontend on `xjit` for the Neo Geo, CPS1 (mame-go) and Mega Drive
(gwenesis) emulators of esp32-emu-turbo. How the frontend works:
[68000 frontend](m68k.md).

## Rules (same as the GBA)

- **Both engines in one app**, the interpreter as the fallback.
- **Automatic fallback** after a crash or hang (per-game state), with a menu switch.
- **Never slower than the interpreter:** the first seconds of a game measure the
  dynarec's time per frame against the interpreter's; a game where the dynarec
  loses runs on the interpreter.
- **Bit-exact first:** every change passes the differential fuzz in QEMU before
  any board run.

## Steps

| Step | What | Where | Status |
|---|---|---|---|
| 8a | Instruction lengths, block model on Musashi, host differential fuzz | PC | done |
| 8b | Xtensa blocks (handler calls), `test/m68k-test` in QEMU | QEMU | done |
| 8c | Native register-only instructions | QEMU | done |
| 8d | Native Bcc / DBcc | QEMU | done |
| 8e | Memory operands | QEMU | done |
| 8f | Indexed modes, PEA/JSR/BSR/RTS, shifts, MOVEM | QEMU | done |
| 8g | Block chaining | QEMU | done |
| 9a | Glue for mame-go's Musashi 3.1; the fuzz against it (PC and QEMU, idle-loop skip included) | PC + QEMU | done |
| 9b | mame-go behind `M68KJIT`: fixed-ROM ranges from the 68000's read map, partition room | board | done |
| 9c | Board correctness: the interpreter's screen hashes on Metal Slug | board | done (7 of 7) |
| 9d | Board speed against the interpreter | board | done: **slower** (13.1 ms against 7.7 ms a frame) |
| 9f | Compact code generation, F0-F6 ([plan](codegen-plan.md)) | QEMU + board | F0-F2 done, F3-F6 on hold |
| 9e, 9g | Fallback and speed guard per game, compatibility runs | board | not started: nothing to guard while the dynarec is off |
| 10 | gwenesis (Mega Drive) | board | not started |

**On hold since 2026-10-03.** The dynarec is exact and slower than the
interpreter on the board; the reasons, the numbers and what would reopen it are
in [68000 in mame-go](mame.md).

Lessons from the GBA that apply: hot C helpers in IRAM, core-1 work out of core
0's way, the internal RAM budget, the translated code running from PSRAM through
the shared 32 KB instruction cache.
