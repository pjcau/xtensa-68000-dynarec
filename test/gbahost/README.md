# gbahost

The board's gpSP sources built and run natively — no Docker, no cross compiler,
no QEMU — so that a change to the GBA hardware model or to the scanline
renderer can be weighed and proved before it is flashed.

    ./build.sh                       # build/gbahost and build/videobench

The host CPU is not the LX7. What carries over from a measurement here is the
*work removed* (pixels written, stores issued, instructions in the hot loop)
and the share one part of a frame takes of the whole; absolute milliseconds do
not.

**How much the host bench can resolve.** Not much, on the heavy scenes. Adding
256 bytes of dead code to `video.cpp` — code the renderer never calls — moves
`mode0-blend` by 10 %, `mode0-4bg` and `mode0-objs` by 6 %: what moved is the
alignment of the hot loops against the host's caches and branch predictor, not
the work. Read a host difference under about 10 % on those scenes, or 3 % on
the light ones, as nothing at all, and check the same way (a dead-code build)
before believing one. Two further traps the harness has already fallen into:
the frame hash costs more than drawing a plain layer, so it is kept out of the
timed loop; and one path too many in `render_tile_Nbpp` pushed clang past its
inlining threshold and turned it into a call per tile, a 25 % loss that had
nothing to do with the change (hence the `always_inline`, and `nm build/obj/video.o
| c++filt | grep render_tile_Nbpp` to check — it should print nothing).

**And the instruction count did not predict the board either.** On 2026-10-09
three renderer changes that each removed instructions from the innermost pixel
loop — 7 a pixel down to 4, 56 a tile row down to 37, proven by `xtcount.sh`,
with identical frames and audio — were measured on the board and made the
renderer *slower* on all three games: `render` per drawn frame +0.37 ms on
Mario Kart, +0.27 on Sonic Advance, +0.13 on Metal Slug Advance, above the
baseline in every one of 20 seconds, on every game. The `GBAROWS` counters
showed the new path was being taken on 65-98 % of the rows it was written for,
so it fired as intended and still lost. All three were dropped (fork branch
`gba-speed`, kept as a record, never merged).

What that means for anyone measuring here: on this chip the instruction count
of a hot loop is *not* the cost model. An extra test per row plus a second
copy of the loop costs more in instruction-cache and branch behaviour than the
per-pixel test it removes, and nothing available on this machine predicts that.
A host figure is noise; an instruction count is necessary but not sufficient.
**Only the board decides a renderer change.** Use `xtcount.sh` to know what a
change does to the work, and the board to know whether it helps.

So for a change that trades instructions per pixel, the count below is
necessary evidence but not sufficient:

    ./xtcount.sh ../../components/gbsp-libretro/video.cpp

which compiles one gpSP source with the IDF's Xtensa compiler and prints the
instructions of each function and the total text size. The LX7 issues one
instruction at a time, in order, with no branch predictor, so a loop that
executes fewer instructions costs fewer cycles.

## gbahost — a ROM, frame by frame

    ./build/gbahost <rom.gba> <frames>

prints a video and an audio hash over the whole run and the milliseconds a
frame spent in the ARM interpreter (with the scanline renderer's share of it),
and in the sound read-out. Options come from the environment:

| | |
|---|---|
| `LOADFILE=<state>` | resume a save state, so frame 0 is already in play |
| `SAVEAT=n SAVEFILE=f` | write a save state after frame n |
| `INPUT=a-b:mask,...` | keys held by frame range (libretro joypad bit mask) |
| `DUMPAT=n DUMPFILE=f` | write frame n as a PPM |
| `NODRAW=1` | emulate without drawing |
| `TRACE=1` | a hash per frame, to find the first frame that differs |
| `REPEAT=n` | run the frame loop n times and keep the fastest |
| `PROF=1` | a flat PC histogram of the host, symbolised by `prof.py` |
| `PCHIST=n` | the n hottest *guest* PCs and a candidate idle loop (needs `DEFS=-DPCHIST=1`) |
| `IDLEPC=<hex>` | the game's idle loop, as `gba_over.h` would give it |

    PROF=1 REPEAT=20 ./build/gbahost game.gba 400 | ./prof.py build/gbahost

The interpreter runs here, not the dynarec, so `execute_arm` stands where the
translated code stands on the board; everything it calls — the memory
handlers, the timers, DMA, the sound mixer, the renderer — is the same code.

ROMs are not part of this repository.

### Finding a game's idle loop

The one lever that removes *guest* work rather than host work, so the only one
whose size does not depend on the machine measuring it. A GBA game finishes its
frame and then spins waiting for VBlank; the emulator runs that spin at full
cost unless `gba_over.h` gives the game an `idle_loop_target_pc`, at which
point gpSP stops emulating the rest of the frame when the loop is reached.
200 games have an entry. Most do not.

    DEFS=-DPCHIST=1 ./build.sh
    LOADFILE=g.state INPUT=0-99999:256 PCHIST=30 ./build/gbahost g.gba 600

prints the hottest guest PCs with their share, and names a candidate: the
hottest PC plus every PC within 32 bytes of it that runs at least half as
often — one short loop — and the lowest PC of that run, which is the one the
loop branches back to and the one `gba_over.h` wants. Then measure it:

    IDLEPC=0x800096c LOADFILE=g.state INPUT=0-99999:256 ./build/gbahost g.gba 600

**`guest instructions/frame` is the number that matters** — that is the work
the board stops emulating. On Celeste Classic (no `gba_over.h` entry) the
candidate at `0x08000190` takes it from 19903 to 10774 a frame, −46 %.

The frame hash is **not** the acceptance test here, and must not be used as
one: skipping the spin moves the guest's cycle counter, so DMA, timers and the
audio interleave differently and every hash changes even when the game plays
identically. Judge it on the picture instead — `DUMPAT`/`DUMPFILE` a frame with
and without, and expect a difference of a fraction of a per cent, one animation
phase (Celeste: 264 bytes of 115200, 0.2 %) — and then on the board, by
playing the game and checking the buttons. A wrong PC breaks the game visibly.

The interpreter runs here, not the dynarec, and that is exactly what makes this
measurement right: gpSP gives the guest its 280896 cycles a frame either way,
so a frame executes the same guest instructions under both engines, and an
entry found here helps both.

## videobench — the renderer alone

    ./build/videobench list                 # the scenes
    ./build/videobench bench all 200        # time each scene
    ./build/videobench fuzz 400             # a hash per random display setup

No ROM: VRAM, OAM, the palette and the display registers are filled from a
seeded generator and `update_scanline()` draws 160 lines through the same
snapshot path the board uses. `bench` gives a microsecond figure per scene
(the frame hash is never inside the timed loop: one serial multiply a pixel
costs more than drawing a plain layer); `fuzz` draws hundreds of random display configurations — modes, layer
counts, 4bpp and 8bpp, mosaic, windows, blending, rotated sprites — and prints
a hash per case. Two builds whose `fuzz` output is identical draw the same
picture, which is what a renderer change has to prove.
