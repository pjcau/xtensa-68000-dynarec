# 68000 roadmap

The 68000 frontend on `xjit` for the Neo Geo, CPS1 (mame-go) and Mega Drive
(gwenesis) emulators of esp32-emu-turbo. Those games run at 40-60 fps on the
interpreter today, and there the CPU is most of the cost. How the frontend works:
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
| 9 | mame-go (Neo Geo, CPS1): Musashi 3.1 glue, build switch, fallback, speed guard, games on the board with the webcam | board | next |
| 10 | gwenesis (Mega Drive), if it helps there (the VDP is its main cost) | board | |

Lessons from the GBA that apply: hot C helpers in IRAM, core-1 work out of core
0's way, the internal RAM budget, the translated code running from PSRAM through
the shared 32 KB instruction cache.
