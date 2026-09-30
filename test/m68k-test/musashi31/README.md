# Musashi 3.1 (mame-go) for the fuzz

Musashi 3.1 is "free for non-commercial use": it is **not** in this repository.
The fuzz compiles the copy in the emulator's own tree, `MUSASHI31_DIR`
(`retro-go/mame-go/components/mame2000/src/cpu/m68000` in the retro-go fork),
with the stand-ins for MAME's headers in this directory:

- `cpuintrf.h`, `memory.h`, `retro_inline.h`: the memory calls go to the fuzz's
  memory, `change_pc32()` is counted (the count is compared between engines);
- `musashi31_all.c`: `m68kcpu.c` plus a reset of its idle-loop state, which
  the fuzz needs between the two runs of a seed;
- `shim_dasm.c`: the instruction check the generator uses (Musashi 4.5's).

Build with `-DMAMEGO -DFUZZ_MUSASHI31` (see `../host/run_host_checks.sh` and the
QEMU build in `../main/CMakeLists.txt`).
