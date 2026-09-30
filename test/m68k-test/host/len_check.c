/* len_check: m68kjit_insn_len() against Musashi's disassembler, for every
 * opcode valid on the 68000 and a few extension-word patterns each. Bit 8
 * of an index word stays 0: Musashi's disassembler reads it as the 68020
 * full format, which the 68000 does not have. */
#include <stdio.h>
#include <stdint.h>
#include "m68k.h"
#include "m68kjit.h"

static uint8_t mem[16];
unsigned int m68k_read_disassembler_8(unsigned int a) { return mem[a & 15]; }
unsigned int m68k_read_disassembler_16(unsigned int a) { return mem[a & 15] << 8 | mem[(a + 1) & 15]; }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_disassembler_16(a) << 16 | m68k_read_disassembler_16(a + 2); }

int main(void)
{
    static const uint8_t ext[][6] = {{0,0,0,0,0,0}, {0x12,0x34,0x56,0x78,0x9A,0xBC}, {0xFE,0x7F,0xFE,0x7F,0xFE,0x7F}, {0x80,0x01,0x00,0x02,0x00,0x04}};
    int valid = 0, bad = 0;
    char s[256];
    for (unsigned op = 0; op < 0x10000; op++)
    {
        /* line A and F trap on the 68000 (Musashi's check lets the 68030 MMU ops through) */
        if (!m68k_is_valid_instruction(op, M68K_CPU_TYPE_68000) || (op >> 12) == 0xA || (op >> 12) == 0xF) continue;
        valid++;
        for (unsigned k = 0; k < sizeof(ext) / sizeof(ext[0]); k++)
        {
            mem[0] = op >> 8; mem[1] = op;
            for (int i = 0; i < 6; i++) mem[2 + i] = ext[k][i];
            int want = m68k_disassemble(s, 0, M68K_CPU_TYPE_68000);
            int got = m68kjit_insn_len(op);
            if (want != got && bad++ < 40) printf("%04X ext%u: len %d, Musashi %d  %s\n", op, k, got, want, s);
        }
    }
    printf("LEN %d valid opcodes, %d mismatches\n", valid, bad);
    return bad != 0;
}
