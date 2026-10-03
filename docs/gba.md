# Game Boy Advance dynarec

An Xtensa backend for [gpSP](https://github.com/libretro/gpsp)'s translator
(ARM7TDMI and Thumb), in `components/gbsp-libretro/xtensa/`. It is the engine of
the GBA emulator on the esp32-emu-turbo handheld. The code is finished and in
use.

![The stack](img/stack.svg)

## Status

| | |
|---|---|
| Correctness | Video **and** audio hashes bit-identical to gpSP's own x86 dynarec (QEMU, Sonic Advance, Metal Slug Advance, TMNT, 300/600 frames of level save states) |
| Speed | 56-60 emulated fps in play on the board; 68-88 fps-equivalent of CPU headroom at steady state (deterministic benchmark) |
| Compatibility | 11 of 12 tested games on the dynarec, correct graphics (webcam checked); NFS Underground falls back to the interpreter automatically |
| Safety | Automatic interpreter fallback on self-modifying-code storms and after a crash/hang; save states and battery saves are engine-independent |

| Game (played, board) | Emulated fps | Shown fps |
|---|---|---|
| Sonic Advance | 58.8 | 54.5 |
| Metal Slug Advance | 55.7 | 55.6 |
| TMNT | 59.5 | 58.7 |
| GTA, Pokemon Fire Red (IT), Prince of Persia, Sonic Advance 2, SF Alpha 3, SMA4, Millionaire Jr | ~60 in play | ~60 |
| Mario Kart Super Circuit (race) | 44-51 | 44-51 |
| NFS Underground (interpreter fallback) | 13 | 13 |

## Where the pieces are

| Path | What |
|---|---|
| `components/gbsp-libretro/xtensa/xtensa_emit.h`, `xtensa_emit_ops.h` | what each gpSP translator operation becomes in Xtensa code |
| `components/gbsp-libretro/xtensa/xtensa_stub.c` | the hand-written entry, exit and memory stubs |
| `components/xjit/` | the encoders, labels, literal pools and executable memory the backend uses |
| `test/gbajit-test/` | gpSP in QEMU (ESP32-S3, 8 MB octal PSRAM) from a ROM and a level state; `x86ref/` builds the x86 dynarec as the reference |

The retro-go app that runs it (`gbsp/main/main.c`: frame loop, three frame
buffers, per-game engine state, battery saves, `GBAPROF` / `GBABENCH`) lives in
the retro-go fork, which includes this repository as a submodule.

## Run it in QEMU

ROMs and level states are not part of this repository: point `XJIT_ROMS` at a
directory with `<game>.gba` and `<game>.state`.

    cd test/gbajit-test
    ./docker.sh "GBAJIT=1 idf.py -B build_jit -DSDKCONFIG=build_jit/sdkconfig build"
    ./docker.sh "B=build_jit ./run.sh sonic 400"
    # GBAJIT frames 300 hash 7ce642f7 audio 3a63b955 ...  (== x86ref/refs_x86jit.txt)

## More

- [Architecture](architecture.md): where each piece lives in memory, how a frame runs, the host register map, the code shape
- [Verification](verification.md): encoder tests, QEMU against the x86 reference, board benchmark, compatibility runs
- [Performance](performance.md): what each step gave, the LX7 counters, what did not help and why
- [Fallback and saves](fallback.md): when and how a game switches to the interpreter
