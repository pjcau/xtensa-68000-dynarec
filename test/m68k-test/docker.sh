#!/bin/bash
# Run a command in the espressif/idf:v5.4 container from this directory, with a newer
# Espressif QEMU first in PATH (see README.md). Example: ./docker.sh "idf.py build && ./qemu.sh 15"
QEMU_DIR=${XJIT_QEMU_DIR:-$HOME/Documents/myProjects/esp32-emu-turbo-scratch/qemu}
# With XJIT_IMAGE=xjit-idf (docker/Dockerfile) QEMU is already inside the image.
[ -d "$QEMU_DIR" ] && QMOUNT="-v $QEMU_DIR:/q" || QMOUNT=
# MUSASHI31_DIR=<mame-go's cpu/m68000>: build against Musashi 3.1 (never copied into this repo)
[ -n "$MUSASHI31_DIR" ] && QMOUNT="$QMOUNT -v $(cd "$MUSASHI31_DIR" && pwd):/musashi31:ro -e MUSASHI31_DIR=/musashi31"
exec docker run --rm -v "$PWD/../..":/rg $QMOUNT -w /rg/test/m68k-test ${XJIT_IMAGE:-espressif/idf:v5.4} \
  bash -c ". /opt/esp/idf/export.sh >/dev/null 2>&1; export PATH=/q/qemu/bin:\$PATH; $*"
