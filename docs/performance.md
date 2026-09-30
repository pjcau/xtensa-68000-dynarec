# Performance

Board: ESP32-S3 N16R8 at 240 MHz, ILI9488 LCD on a 20 MHz 8-bit bus, GBA picture
scaled 2x to 480x320.

## What each step gave

| Change | Effect |
|---|---|
| first correct board run | Metal Slug ~29 fps (interpreter 46) |
| direct aligned loads/stores instead of `memcpy` | ~29 → ~31 fps |
| 16-bit density instructions | ~31 → ~35 fps |
| hot C helpers in IRAM (`XT_HOT`) | ~35 → 40-45 fps |
| three frame buffers (emulation stops waiting for the LCD) | Sonic 49.7 → 57.5 emulated fps |
| renderer VRAM copy (no core-0 wait on VRAM writes) | CPU time per frame −9 to −13 % (Sonic, TMNT) |
| render task below the display task | Sonic 46.6 → 54.5 shown fps |
| 512-entry block-lookup cache in internal RAM | −1.3 % |
| m4a mixer on locals instead of references | −1 to −2.5 % |
| cold code out of line, compact stores, `r0`-`r2` in registers | 0-2 % each |

## What the counters say

On a Sonic window the dynarec executes 1.3 M host instructions per frame against
the interpreter's 2.0 M, at 2.4 cycles per instruction (interpreter 1.6), with
~0.8 M cycles per frame of extra instruction-fetch stall: the translated code
comes from PSRAM. The translated code itself is 29-34 % of core 0 in most games
(58 % in Mario Kart's races); the rest is the GBA hardware model in C (timers, DMA,
sound, memory handlers, block lookup). That is why code-generation tweaks now
give little, and moving hot C code to IRAM gave a lot.

## Hard limits

- **The LCD bus:** a full-screen 2x frame is ~307 KB, 15.4 ms at 20 MHz (already
  above the ILI9488's rated write cycle). Scrolling games cannot show much more
  than ~60 fps even with a free core 1.
- **The I-cache:** 32 KB, shared by both cores and by flash code. A translated-code
  cache in internal RAM would remove the PSRAM fetch stalls, but with the app's
  internal RAM use it does not fit (the file system needs the RAM).

## What did not help

- A 32-64 KB translation cache in internal RAM: does not fit (see above).
- Drawing lines into an internal-RAM buffer: both cores ~5 % slower (SRAM bank
  contention).
- Skipping the display's per-line checksums while scrolling: every frame went
  out full, slower.
- Waking the renderer every line instead of every 8: the notifications cost more
  than the lag they save.
