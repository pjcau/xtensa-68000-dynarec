# xtensa-68000-dynarec

Dynamic recompilers (JIT) for retro CPUs on the **ESP32-S3** (Xtensa LX7, 240 MHz,
32 KB instruction cache, 8 MB PSRAM), built for the
[esp32-emu-turbo](https://github.com/pjcau/esp32-emu-turbo) handheld and its
[retro-go](https://github.com/pjcau/retro-go) fork.

Two frontends share one small JIT core, `xjit` (Xtensa instruction encoders,
labels, literal pools, executable memory):

| | Game Boy Advance (ARM7TDMI / Thumb) | Motorola 68000 (Neo Geo, CPS1, MAME) |
|---|---|---|
| What | an Xtensa backend for [gpSP](https://github.com/libretro/gpsp) | `m68kjit`, blocks on top of the Musashi interpreter |
| Code | finished | finished |
| Correct | video and audio bit-identical to gpSP's x86 dynarec | instruction-for-instruction equal to Musashi; the board gives the interpreter's screen hashes |
| Speed on the board | **56-60 fps in play**, 11 of 12 tested games | **slower than the interpreter** (13.1 ms against 7.7 ms of 68000 a frame on Metal Slug) |
| State | **in use**: the engine of the handheld's GBA emulator | **switched off, on hold**: the interpreter is the engine, and it was made faster instead |
| Details | [docs/gba.md](docs/gba.md) | [docs/mame.md](docs/mame.md) |

Both follow the same rules: both engines in one app, automatic fallback to the
interpreter, never slower than the interpreter (a game where the dynarec loses
runs on the interpreter), and bit-exact first (every change passes a
differential test in QEMU before it touches the board).

Why one works and the other does not, in one line: translated code is fetched
from PSRAM through a 32 KB instruction cache that both cores and all the
firmware share. The GBA's hot code per frame is small enough; a Neo Geo game's
is not (635 KB of translated code for Metal Slug). [docs/mame.md](docs/mame.md)
has the measurements and what would have to change.

| GBA | 68000 in mame-go |
|---|---|
| ![The GBA stack](docs/img/stack.svg) | ![The 68000 stack](docs/img/m68k-stack.svg) |

## Layout

| Path | What |
|---|---|
| `components/xjit/` | The shared JIT core: `xjit_emit.h` (Xtensa encoders, byte-identical to `xtensa-esp32s3-elf-as`), `xjit_block.c` (labels, literal pool), `xjit_exec.c` (executable IRAM / PSRAM memory, cache sync) |
| `components/gbsp-libretro/` | gpSP with the Xtensa backend in `xtensa/` and the esp32-emu-turbo changes (two-core renderer, VRAM copy, m4a mixer HLE, battery-save flags) |
| `components/m68kjit/` | The 68000 frontend: instruction lengths, block cache, dispatcher, native translation and chaining; `glue/glue_musashi31.c` plugs it into mame-go's Musashi 3.1 |
| `test/xjit-test/` | Encoder tests against the assembler, generated-code tests in QEMU and on the board |
| `test/gbajit-test/` | gpSP in QEMU from a ROM and a level state; `x86ref/` builds gpSP's x86 dynarec as the bit-exact reference |
| `test/m68k-test/` | The 68000 differential fuzz against Musashi 4.5 (vendored, MIT) and 3.1: on the PC (C model, mutations) and in QEMU (Xtensa code) |
| `docs/` | One page per frontend, then the shared and the detailed pages below |

## Documentation

Start with the page of the frontend you care about:

- **[GBA](docs/gba.md)**: status, results per game, how to run it in QEMU.
- **[68000 in mame-go](docs/mame.md)**: how it plugs into MAME, what was measured on the board, why it is on hold, what was done to the interpreter instead.

Shared and detailed pages:

- [Architecture](docs/architecture.md) (GBA): the pieces, where each lives in memory, how a frame runs, the host register map
- [Verification](docs/verification.md) (GBA): encoder tests, QEMU against the x86 reference, board benchmark
- [Performance](docs/performance.md) (GBA): what each step gave, the LX7 counters, what did not help
- [Fallback and saves](docs/fallback.md) (GBA): when and how a game switches to the interpreter
- [68000 frontend](docs/m68k.md): blocks on top of Musashi, native instructions, chaining, verification
- [68000 roadmap](docs/roadmap-68000.md): the steps and where each stopped
- [68000 code generation plan](docs/codegen-plan.md): steps F0-F6 towards denser code, F0-F2 done

## Quick start (QEMU, no board)

Needs Docker. The image in `docker/` has Espressif's QEMU with ESP32-S3 PSRAM
support:

    docker build -t xjit-idf docker/
    export XJIT_IMAGE=xjit-idf

The 68000 fuzz needs no ROM:

    cd test/m68k-test
    ./docker.sh "idf.py set-target esp32s3 && idf.py build && ./qemu.sh 600"   # see test/m68k-test/README.md

The GBA run needs a ROM and a level state, which are **not** in this repository
(see [docs/gba.md](docs/gba.md)).

## License and credits

GPL-2.0 (see `COPYING`), as gpSP.

- **gpSP** by Exophase, and the libretro/gpsp contributors (David Guillen Fandos and
  others): the emulator, the translator and the x86/ARM/MIPS backends this Xtensa
  backend follows.
- **retro-go** by Alex Duchesne: the ESP32 frontend the emulators run in.
- **Dragonfruit** (MIT, Kevin Fisher): reference for a few Xtensa encodings and for
  a 68000 JIT on the ESP32-S3.
- **Musashi** by Karl Stenerud (MIT since 4.x): the 68000 interpreter `m68kjit` builds
  on; version 4.5 is vendored in `test/m68k-test/musashi` as the fuzz reference, with
  John R. Hauser's SoftFloat it depends on.
- The Xtensa backend, `xjit`, `m68kjit` and the test benches: written for esp32-emu-turbo.
