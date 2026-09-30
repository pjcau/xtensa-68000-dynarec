# xjit-test

Standalone ESP-IDF test app for the shared Xtensa JIT core (`components/xjit`),
step by step as in the [JIT plan](https://github.com/pjcau/esp32-emu-turbo/blob/main/website/docs/next-steps/jit-plan.md). It is not one of
`rg_tool.py`'s apps and changes no emulator.

## QEMU

The QEMU in the `espressif/idf:v5.4` image (esp_develop 9.0.0) has no
ESP32-S3 PSRAM. Use esp_develop 9.2.2 or newer, unpacked outside the repo:

    mkdir -p ~/Documents/myProjects/esp32-emu-turbo-scratch/qemu && cd $_
    curl -sL https://github.com/espressif/qemu/releases/download/esp-develop-9.2.2-20260417/qemu-xtensa-softmmu-esp_develop_9.2.2_20260417-x86_64-linux-gnu.tar.xz | tar xJ

(`XJIT_QEMU_DIR` points `docker.sh` elsewhere.)

## Build and run

    ./docker.sh "idf.py set-target esp32s3 && idf.py build"
    ./docker.sh "./qemu.sh 15"          # 8 MB octal PSRAM, like the board

Results are `XJIT ...` lines. QEMU checks correctness only: cycle counts
mean something on the board alone. On the board, `./board.sh` borrows the
opentyrian-go partition through the launcher's SD update, runs the test once
(it reboots into the launcher) and puts opentyrian-go back; no USB flash, no
emulator touched.

## Step 0 (2026-09-29)

In QEMU, code written at run time runs from internal RAM
(`MALLOC_CAP_EXEC`, memory protection off) and from PSRAM mapped for
instruction fetch (`esp_mmu_map(..., MMU_MEM_CAP_EXEC)` after
`esp_cache_msync` C2M on the data alias and M2C/INST on the code alias).
Xtensa note: `entry` must sit at a 4-byte aligned address.

## Steps 1-3 (2026-09-29)

- **Step 1**: `components/xjit/include/xjit_emit.h` matches `xtensa-esp32s3-elf-as`
  byte for byte on ~125 KB of random instructions (`components/xjit/test/run_emit_test.sh`).
- **Step 2**: blocks built with `xjit_block.h` (labels, literal pool, calls into
  C, 26 branch kinds, loads/stores) and 2000 random ALU programs x 8 inputs
  against a C reference: 16/16 in QEMU and on the board, from internal RAM and
  from PSRAM. On the board the instruction-cache invalidate must cover whole
  lines (an unaligned range is refused): QEMU could not show it.
- **Step 3** (board, 240 MHz): generated ALU code 1.0-1.3 cycles/op (gcc -O2
  C: 0.94); block call+return 13 cycles; callx8 into C 14 cycles; 48 KB of
  straight-line code (bigger than the 32 KB I-cache) 1.0 cycles/op from
  internal RAM but 11.6 from PSRAM.
