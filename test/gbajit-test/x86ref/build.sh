#!/bin/bash
# gbahost with gpSP's x86 dynarec (32-bit, in the gpsp-i386 container): the reference for the Xtensa backend.
# Same sources as retro-go/gbsp (mounted at /g), without RETRO_GO (its dynamic VRAM/EWRAM do not fit
# the x86 stub); harness host_jit.c. Output: ./gbahost-x86 in the current directory.
# Image: docker build -t gpsp-i386 . (from this directory). Hashes: refs_x86jit.txt
docker run --rm -v $(cd "$(dirname "$0")/../../../components/gbsp-libretro" && pwd):/g \
  -v $(cd "$(dirname "$0")" && pwd):/w -v $PWD:/o -w /o -e DEFS="$DEFS" -e EXTRA="$EXTRA" gpsp-i386 bash -c '
set -e
F="-O2 -g -w -fomit-frame-pointer -ffast-math -DOVERCLOCK_60FPS -DROM_BUFFER_SIZE=8 ${DEFS:--DIRAM_ATTR=} -DHAVE_DYNAREC -DX86_ARCH -DMMAP_JIT_CACHE ${EXTRA:--msse2 -mfpmath=sse} -I/w -I/g -I/g/libretro/libretro-common/include"
mkdir -p obj; O=""
for s in cheats.c cpu.cpp cpu_threaded.c gba_cc_lut.c gba_memory.c gbp.c input.c main.c memmap.c rfu.c savestate.c serial.c sound.c video.cpp x86/x86_stub.S; do
  o=obj/$(basename ${s%.*}).o; O="$O $o"
  case $s in *.cpp) g++ $F -c /g/$s -o $o ;; *) gcc $F -c /g/$s -o $o ;; esac || { echo FAILED $s; exit 1; }
done
gcc $F -c /w/host_jit.c -o obj/host.o && g++ $F obj/host.o $O -lm -o gbahost-x86 && echo BUILT'
