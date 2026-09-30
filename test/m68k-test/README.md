# m68k-test

The 68000 frontend (`components/m68kjit`) against the Musashi interpreter it
runs on (Musashi 4.5, MIT, vendored in `musashi/`, see `VENDORED.txt`).

`main/fuzz.c` builds a random machine (1 MB over the 24-bit bus: 256 KB of ROM
with random but valid 68000 instruction streams and a vector table into it,
RAM above), runs it for 32 time slices with random interrupt levels once with
Musashi's `m68k_execute()` and once with the dynarec (`glue_jit_execute()`),
and compares D0-D7, A0-A7, PC, SR, USP, ISP, the cycles used and left, and a
hash of the RAM after every slice.

## On the PC (C backend, no ESP-IDF)

    host/run_host_checks.sh [seeds]

1. `m68kjit_insn_len()` against Musashi's disassembler on every opcode valid on
   the 68000;
2. the fuzz with the C backend (the model of the generated code);
3. mutations of the block engine that the fuzz must catch.

## In QEMU (Xtensa backend)

    ./docker.sh "idf.py set-target esp32s3 && idf.py build && ./qemu.sh 600"

(`XJIT_IMAGE=xjit-idf` for the image with QEMU inside, see `docker/`.) The app
runs 20 seeds with translation off (the harness alone), 300 seeds of random
valid code, 300 seeds whose ROM is 85 % instructions the translator handles
natively (`fuzz_native_bias`), then 20 seeds for each of the 23 instruction
families alone (`fuzz_form`). Half the D registers start at edge values (0, -1,
0x80000000, 0x7FFF, ...), and in the native passes a `MOVE SR,-(A7)` follows
half the instructions, so the RAM hash sees their flags:

    M68K harness (no translation) 20 seeds, 0 mismatches
    M68K FUZZ 300 seeds, 0 mismatches; blocks 4673 insns 91878 native 74384 runs 102576 ...
    M68K NATIVE-HEAVY 300 seeds, 0 mismatches; native 49494 runs 91168 links 3927
    M68K BY-FORM 23 families x 20 seeds, 0 mismatches

`native` counts translated instructions done natively, `runs` the block runs
started by the dispatcher (chained runs are not counted), `links` the chained
exits. Before a change is committed, one deliberate bug in the code it touches
must make this fail (see `docs/m68k.md`, Verification).
`-DCMAKE_C_FLAGS=-DM68KJIT_TRACE` prints the first block runs.

Musashi 4.5's opcode and cycle tables (~800 KB) live in PSRAM here
(`main/linker.lf`); the emulators keep their own Musashi.
