#!/bin/bash
# run the built xjit-test in Espressif's QEMU (ESP32-S3 machine, 8 MB octal PSRAM like the board),
# serial on stdout, for $1 seconds; B=<build dir> (default build). Needs QEMU esp_develop 9.2.2 or
# newer: the 9.0.0 in the espressif/idf:v5.4 image has no ESP32-S3 PSRAM.
cd ${B:-build} && python -m esptool --chip=esp32s3 merge_bin --fill-flash-size 16MB -o qemu_flash.bin @flash_args >/dev/null
[ -f qemu_efuse.bin ] || head -c 1024 /dev/zero > qemu_efuse.bin
timeout ${1:-20} qemu-system-xtensa -M esp32s3 -nographic -no-reboot \
  -drive file=qemu_flash.bin,if=mtd,format=raw \
  -drive file=qemu_efuse.bin,if=none,format=raw,id=efuse \
  -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
  -m 8M -global driver=ssi_psram,property=is_octal,value=true \
  -serial mon:stdio ${QEMU_EXTRA}
