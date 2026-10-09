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

So for a change that trades instructions per pixel, the figure that decides is
the instruction count on the board's own CPU:

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
| `PROF=1` | a flat PC histogram, symbolised by `prof.py` |

    PROF=1 REPEAT=20 ./build/gbahost game.gba 400 | ./prof.py build/gbahost

The interpreter runs here, not the dynarec, so `execute_arm` stands where the
translated code stands on the board; everything it calls — the memory
handlers, the timers, DMA, the sound mixer, the renderer — is the same code.

ROMs are not part of this repository.

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
