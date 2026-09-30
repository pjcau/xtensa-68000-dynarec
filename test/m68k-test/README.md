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

    ./docker.sh "idf.py set-target esp32s3 && idf.py build && ./qemu.sh 400"

(`XJIT_IMAGE=xjit-idf` for the image with QEMU inside, see `docker/`.) The app
runs 20 seeds with translation off (the harness alone), 300 seeds of random
valid code, then 300 seeds whose ROM is 85 % instructions the translator
handles natively (`fuzz_native_bias`):

    M68K harness (no translation) 20 seeds, 0 mismatches
    M68K FUZZ 300 seeds, 0 mismatches; blocks 5217 insns 104207 native 12484 runs 132523 ...
    M68K NATIVE-HEAVY 300 seeds, 0 mismatches; native 72302 runs 158069

The block, instruction and run counts of the first pass must equal the host run
of the same seeds (`host/fuzz_host 300`): the generated code leaves its blocks
exactly where the C model does.
`-DCMAKE_C_FLAGS=-DM68KJIT_TRACE` prints the first block runs.

Musashi 4.5's opcode and cycle tables (~800 KB) live in PSRAM here
(`main/linker.lf`); the emulators keep their own Musashi.
