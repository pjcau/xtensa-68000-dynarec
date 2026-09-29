# xjit-test

Standalone ESP-IDF test app for the shared Xtensa JIT core (`components/xjit`),
step by step as in `website/docs/next-steps/jit-plan.md`. It is not one of
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
mean something on the board alone (`idf.py flash` from the host).

## Step 0 (2026-09-29)

In QEMU, code written at run time runs from internal RAM
(`MALLOC_CAP_EXEC`, memory protection off) and from PSRAM mapped for
instruction fetch (`esp_mmu_map(..., MMU_MEM_CAP_EXEC)` after
`esp_cache_msync` C2M on the data alias and M2C/INST on the code alias).
Xtensa note: `entry` must sit at a 4-byte aligned address.
