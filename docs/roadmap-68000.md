# 68000 roadmap

The next frontend on `xjit`: the Motorola 68000 of the Neo Geo, CPS1 (mame-go) and
Mega Drive (gwenesis) emulators of esp32-emu-turbo. Those games run at 40-60 fps
on the interpreter today, and there the CPU is most of the cost.

## Rules (same as the GBA)

- **Both engines in one app**, the interpreter as the fallback.
- **Automatic fallback** on self-modifying-code storms and after a crash or hang
  (per-game state), with a menu switch.
- **Never slower than the interpreter:** the first seconds of a game measure the
  dynarec's time per frame against the interpreter's; a game where the dynarec
  loses runs on the interpreter.
- **Bit-exact first:** a differential test against the MAME 68000 interpreter
  (instruction sequences, then whole frames of a Neo Geo attract mode) in QEMU,
  before any board run.

## Steps

1. 68000 frontend on `xjit` in a test app (decoder, flags, addressing modes),
   differential test on the PC and in QEMU.
2. mame-go (Neo Geo, CPS1) behind a build switch; hashes, then the board with the
   webcam, game by game.
3. gwenesis (Mega Drive).

Lessons from the GBA that apply: hot C helpers in IRAM, core-1 work out of core
0's way, the internal RAM budget, flushes that must not rewrite the code a helper
returns into.
