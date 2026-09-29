#!/bin/bash
# ./run.sh <game> [seconds]: build (inside docker.sh), put ROMS/<game>.gba and ROMS/<game>.state into the
# flash image (partitions rom, state) and run it in QEMU (8 MB octal PSRAM). ROMS defaults to
# ~/Documents/myProjects/esp32-emu-turbo-scratch/gbajit/roms (mounted at /roms by docker.sh).
set -e
G=$1; T=${2:-600}
cd ${B:-build}
python -m esptool --chip=esp32s3 merge_bin --fill-flash-size 16MB -o qemu_flash.bin @flash_args >/dev/null
dd if=/roms/$G.gba of=qemu_flash.bin bs=64k seek=$((0x400000 / 65536)) conv=notrunc status=none
dd if=/roms/$G.state of=qemu_flash.bin bs=64k seek=$((0xC00000 / 65536)) conv=notrunc status=none
[ -f qemu_efuse.bin ] || head -c 1024 /dev/zero > qemu_efuse.bin
timeout $T stdbuf -oL qemu-system-xtensa -M esp32s3 -nographic -no-reboot \
  -drive file=qemu_flash.bin,if=mtd,format=raw \
  -drive file=qemu_efuse.bin,if=none,format=raw,id=efuse \
  -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
  -m 8M -global driver=ssi_psram,property=is_octal,value=true \
  -serial mon:stdio ${QEMU_EXTRA} | grep -a --line-buffered -E "GBAJIT|Guru|abort|panic|PC  "
