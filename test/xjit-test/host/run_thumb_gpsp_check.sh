#!/bin/bash
# Build thumb_gpsp_check on the PC with gpSP's sources (same flags as the ESP32 component) and run it.
#   run_thumb_gpsp_check.sh <any .gba ROM> [sequences]
set -e
H=$(cd "$(dirname "$0")" && pwd)
G=$H/../../gbsp/components/gbsp-libretro
X=$H/../../components/xjit
T=$(mktemp -d)
FLAGS="-O2 -w -fomit-frame-pointer -ffast-math -DOVERCLOCK_60FPS -DROM_BUFFER_SIZE=8 -DRETRO_GO=1 -I$H/stub -I$G -I$G/libretro/libretro-common/include -I$H/../main -I$X/include"
OBJS=""
for s in cheats.c cpu.cpp gba_cc_lut.c gba_memory.c gbp.c input.c main.c memmap.c rfu.c savestate.c serial.c sound.c video.cpp; do
  o=$T/${s%.*}.o; OBJS="$OBJS $o"
  case $s in *.cpp) g++ $FLAGS -c $G/$s -o $o ;; *) gcc $FLAGS -c $G/$s -o $o ;; esac &
done; wait
gcc $FLAGS -c $H/../main/thumb.c -o $T/thumb.o
gcc $FLAGS -c $X/xjit_block.c -o $T/xjit_block.o
gcc $FLAGS -c $H/thumb_gpsp_check.c -o $T/check.o
g++ $FLAGS $T/check.o $T/thumb.o $T/xjit_block.o $OBJS -lm -o $T/check
$T/check "$@"
