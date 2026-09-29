# gbajit-test

JIT step 5 test bench (`website/docs/next-steps/jit-plan.md`): the gbsp app's gpSP
sources (unchanged, `components/gpsp` compiles them from `../gbsp`) run in QEMU
(or on the board) with a ROM and a level state from flash partitions, and print
`GBAJIT frames N hash H audio A` like the PC harness.

    ./docker.sh "idf.py set-target esp32s3 && idf.py build"
    ./docker.sh "./run.sh sonic 300"      # ROMS/sonic.gba + ROMS/sonic.state

ROMs and states come from `~/Documents/myProjects/esp32-emu-turbo-scratch/gbajit/roms`
(`XJIT_ROMS`); QEMU from `../xjit-test/README.md`.

- QEMU's QIO flash reads (`esp_flash_read`) come back 2 bytes late: this app uses DIO.
- 2026-09-29: the interpreter in QEMU gives the PC reference hashes exactly
  (Sonic, 300 and 600 frames, video and audio).

## The Xtensa dynarec (step 5, 2026-09-29)

    ./docker.sh "GBAJIT=1 idf.py -B build_jit -DSDKCONFIG=build_jit/sdkconfig build"
    ./docker.sh "B=build_jit ./run.sh sonic 150"

builds gpSP with `HAVE_DYNAREC XTENSA_ARCH` (gbsp-libretro/xtensa/, a port of the
x86 backend: ARM state and flags in memory, memory handlers and trampolines in
C) and the translation caches in PSRAM mapped executable. In QEMU its hashes
equal the x86 dynarec's (`x86ref/refs_x86jit.txt`) on Sonic Advance, Metal Slug
Advance and TMNT, video and audio, at 300 and 600 frames. The x86 reference
must use SSE float math: x87 rounds the audio mixer differently.
