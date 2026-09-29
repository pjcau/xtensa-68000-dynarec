#!/bin/bash
# Run a command in the espressif/idf:v5.4 container from this directory, with a newer
# Espressif QEMU first in PATH (see README.md). Example: ./docker.sh "idf.py build && ./qemu.sh 15"
QEMU_DIR=${XJIT_QEMU_DIR:-$HOME/Documents/myProjects/esp32-emu-turbo-scratch/qemu}
exec docker run --rm -v "$PWD/..":/rg -v "$QEMU_DIR":/q -v ${XJIT_ROMS:-$HOME/Documents/myProjects/esp32-emu-turbo-scratch/gbajit/roms}:/roms -w /rg/gbajit-test espressif/idf:v5.4 \
  bash -c ". /opt/esp/idf/export.sh >/dev/null 2>&1; export PATH=/q/qemu/bin:\$PATH; $*"
