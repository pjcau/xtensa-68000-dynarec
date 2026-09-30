#!/bin/bash
# Host checks of the 68000 frontend (no ESP-IDF, no QEMU):
#  1. m68kjit_insn_len() against Musashi's disassembler, every 68000 opcode
#  2. differential fuzz: Musashi's m68k_execute() against the block engine (C backend)
#  3. mutations: the fuzz must catch each deliberate bug in the block engine (a wrong
#     "next" is not one: it only makes a block leave early, the dispatcher resumes)
# ./run_host_checks.sh [fuzz seeds]
set -e
cd "$(dirname "$0")"
M=../musashi; J=../../../components/m68kjit; O=${TMPDIR:-/tmp}/m68kjit-host; mkdir -p $O
CC="gcc -O2 -w -I$M -I$J/include -I../main"
MUSASHI="$M/m68kcpu.c $M/m68kops.c $M/softfloat/softfloat.c $M/m68kdasm.c -lm"
FUZZ="fuzz_host.c ../main/fuzz.c ../main/glue_musashi.c $J/m68kjit_len.c"

$CC -o $O/len_check len_check.c $J/m68kjit_len.c $M/m68kdasm.c && $O/len_check
$CC -o $O/fuzz_host $FUZZ $J/m68kjit.c $MUSASHI && $O/fuzz_host ${1:-1000}

fail=0
while IFS='|' read -r name expr; do
    sed "$expr" $J/m68kjit.c > $O/mut.c
    cmp -s $O/mut.c $J/m68kjit.c && { echo "MUTATION $name: pattern not found"; fail=1; continue; }
    $CC -o $O/fuzz_mut $FUZZ $O/mut.c $MUSASHI
    if $O/fuzz_mut 50 >/dev/null; then echo "MUTATION $name: NOT caught"; fail=1; else echo "MUTATION $name: caught"; fi
done <<'MUT'
no cycle exit|s/if (\*H.cycles <= 0) { m68kjit_stats.exits_cycles++; return; }//
no pc exit|s/if (\*H.pc != i->next) { m68kjit_stats.exits_pc++; return; }//
wrong cycles on ADD|s/i->cyc = H.cyc\[op\];/i->cyc = H.cyc[op] + ((op \& 0xF000) == 0xD000);/
wrong IR (register field)|s/\*H.ir = i->op;/*H.ir = i->op ^ 1;/
MUT
exit $fail
