#!/bin/bash
# Instructions per function in a gpSP source compiled for the board's CPU.
#
# The host bench in this directory cannot resolve a change of a few per cent:
# 256 bytes of dead code added to video.cpp moves its heavier scenes by 10 %,
# because what moves is the alignment of the hot loops, not the work. On the
# LX7 — single issue, in order, no branch predictor — the instructions a hot
# loop executes are the cost, so count them instead:
#
#     ./xtcount.sh ../../components/gbsp-libretro/video.cpp
#
# prints the twelve largest functions and the total text size, which is what a
# before/after pair should be compared on. Needs the IDF's Xtensa compiler
# (xtensa-esp32s3-elf-g++ on PATH, or IDF_TOOLS set below).
set -e
D=$(cd "$(dirname "$0")" && pwd)
G=$D/../../components/gbsp-libretro
IDF_TOOLS=${IDF_TOOLS:-$HOME/.espressif/tools/xtensa-esp-elf}
if ! command -v xtensa-esp32s3-elf-g++ > /dev/null; then
    T=$(find "$IDF_TOOLS" -name xtensa-esp32s3-elf-g++ -print -quit 2> /dev/null || true)
    [ -n "$T" ] || { echo "xtensa-esp32s3-elf-g++ not found (set IDF_TOOLS)"; exit 1; }
    PATH="$(dirname "$T"):$PATH"
fi
O=${OUT:-/tmp/xtcount.o}
# ESP_PLATFORM is left out on purpose: it would pull in the IDF headers, and
# the pixel writers are the same code with or without it
xtensa-esp32s3-elf-g++ -O3 -g -w -fomit-frame-pointer -ffast-math \
    -DOVERCLOCK_60FPS -DROM_BUFFER_SIZE=8 -DRETRO_GO -DXTENSA_ARCH -mlongcalls \
    -I"$D" -I"$G" -I"$G/libretro/libretro-common/include" -c "$1" -o "$O"
xtensa-esp32s3-elf-objdump -d --demangle "$O" | awk '
    /^[0-9a-f]+ <.*>:/ { name = $0; sub(/^[0-9a-f]+ </, "", name); sub(/>:$/, "", name); next }
    /^ *[0-9a-f]+:/ { n[name]++ }
    END { for (k in n) printf "%6d  %s\n", n[k], k }' | sort -rn | head -12
echo "text bytes: $(xtensa-esp32s3-elf-size "$O" | tail -1 | awk '{print $1}')"
