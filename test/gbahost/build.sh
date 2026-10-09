#!/bin/sh
# Native build of the board's gpSP sources (RETRO_GO path, interpreter only):
# no Docker, no cross compiler. `DEFS=...` adds defines, `OUT=...` the build
# directory. Works on macOS/arm64 and on Linux/x86-64.
set -e
D=$(cd "$(dirname "$0")" && pwd)
G=$D/../../components/gbsp-libretro
O=${OUT:-$D/build}
mkdir -p "$O/obj"
F="-O3 -g -w -fomit-frame-pointer -ffast-math -DOVERCLOCK_60FPS -DROM_BUFFER_SIZE=8 -DIRAM_ATTR= -DRETRO_GO ${GBAPROF:--DGBAPROF=1} $DEFS -I$D -I$G -I$G/libretro/libretro-common/include"
OBJ=""
for s in cheats.c cpu.cpp cpu_threaded.c gba_cc_lut.c gba_memory.c gbp.c input.c main.c memmap.c rfu.c savestate.c serial.c sound.c video.cpp; do
    o="$O/obj/$(basename "${s%.*}").o"
    OBJ="$OBJ $o"
    case $s in
        *.cpp) ${CXX:-c++} $F -c "$G/$s" -o "$o" ;;
        *)     ${CC:-cc}   $F -c "$G/$s" -o "$o" ;;
    esac
done
${CC:-cc} $F -c "$D/gbahost.c" -o "$O/obj/gbahost.o"
${CXX:-c++} $F "$O/obj/gbahost.o" $OBJ -lm -o "$O/gbahost"
${CC:-cc} $F -c "$D/videobench.c" -o "$O/obj/videobench.o"
${CXX:-c++} $F "$O/obj/videobench.o" $OBJ -lm -o "$O/videobench"
echo "BUILT $O/gbahost $O/videobench"
