/* m68kjit_len.c - length of a 68000 instruction from its first word.
 *
 * The translator needs the address of the next instruction at translation
 * time. On the 68000 the length depends only on the opcode word (brief
 * extension words have a fixed size; the 68020 full format does not exist),
 * so this is a pure function of the opcode. Checked against Musashi's
 * disassembler for every opcode valid on the 68000 (test/m68k-test/host).
 */
#include "m68kjit.h"

/* extension bytes of an effective address; size 1/2/4 (immediate only) */
static int ea_ext(int mode, int reg, int size)
{
    switch (mode)
    {
    case 5: case 6: return 2;                           /* (d16,An), (d8,An,Xn) */
    case 7:
        switch (reg)
        {
        case 0: return 2;                               /* abs.w */
        case 1: return 4;                               /* abs.l */
        case 2: case 3: return 2;                       /* (d16,PC), (d8,PC,Xn) */
        case 4: return size == 4 ? 4 : 2;               /* #imm */
        }
    }
    return 0;
}

static const int size_ss[4] = {1, 2, 4, 0};             /* 00 byte, 01 word, 10 long */

#define EA(op, size) ea_ext(((op) >> 3) & 7, (op) & 7, size)

int m68kjit_insn_len(uint16_t op)
{
    int ss = (op >> 6) & 3;

    switch (op >> 12)
    {
    case 0x0:
        if (op & 0x0100)
        {
            if (((op >> 3) & 7) == 1) return 4;         /* MOVEP */
            return 2 + EA(op, 1);                       /* BTST/BCHG/BCLR/BSET Dn,<ea> */
        }
        switch ((op >> 9) & 7)
        {
        case 4: return 4 + EA(op, 1);                   /* BTST/BCHG/BCLR/BSET #n,<ea> */
        case 0: case 1: case 5:                         /* ORI, ANDI, EORI (to CCR/SR: 4) */
            if ((op & 0x3F) == 0x3C) return 4;
            /* fall through */
        default:                                        /* SUBI, ADDI, CMPI */
            return 2 + (ss == 2 ? 4 : 2) + EA(op, size_ss[ss]);
        }

    case 0x1: case 0x2: case 0x3:                       /* MOVE, MOVEA */
    {
        int size = (op >> 12) == 1 ? 1 : (op >> 12) == 3 ? 2 : 4;
        return 2 + EA(op, size) + ea_ext((op >> 6) & 7, (op >> 9) & 7, size);
    }

    case 0x4:
        if (op & 0x0100)
            return 2 + EA(op, (op & 0x0040) ? 4 : 2);   /* LEA (ea is an address), CHK.w */
        switch ((op >> 8) & 0xF)
        {
        case 0x0: case 0x2: case 0x4: case 0x6:         /* NEGX CLR NEG NOT; MOVE from SR/to CCR/to SR */
            return 2 + EA(op, ss == 3 ? 2 : size_ss[ss]);
        case 0x8:
            if (ss >= 2 && ((op >> 3) & 7) == 0) return 2;   /* EXT */
            if (ss >= 2) return 4 + EA(op, 2);          /* MOVEM regs,<ea> */
            if (ss == 1 && ((op >> 3) & 7) == 0) return 2;   /* SWAP */
            return 2 + EA(op, 1);                       /* NBCD, PEA */
        case 0xA:
            if (op == 0x4AFC) return 2;                 /* ILLEGAL */
            return 2 + EA(op, ss == 3 ? 1 : size_ss[ss]);    /* TST, TAS */
        case 0xC: return 4 + EA(op, 2);                 /* MOVEM <ea>,regs */
        case 0xE:
            if (ss >= 2) return 2 + EA(op, 4);          /* JSR, JMP */
            switch (op & 0xFFF8)
            {
            case 0x4E50: return 4;                      /* LINK */
            case 0x4E70: return op == 0x4E72 ? 4 : 2;   /* STOP #imm; RESET NOP RTE RTS TRAPV RTR */
            }
            return 2;                                   /* TRAP, UNLK, MOVE USP */
        }
        return 2;

    case 0x5:
        if (ss == 3)
            return ((op >> 3) & 7) == 1 ? 4 : 2 + EA(op, 1);  /* DBcc; Scc */
        return 2 + EA(op, size_ss[ss]);                 /* ADDQ, SUBQ */

    case 0x6: return (op & 0xFF) == 0 ? 4 : 2;          /* Bcc, BRA, BSR */
    case 0x7: return 2;                                 /* MOVEQ */

    case 0x8: case 0xC:                                 /* OR DIVU DIVS SBCD / AND MULU MULS ABCD EXG */
        if (ss == 3) return 2 + EA(op, 2);
        if ((op & 0x0130) == 0x0100) return 2;          /* SBCD/ABCD, EXG */
        return 2 + EA(op, size_ss[ss]);

    case 0x9: case 0xD: case 0xB:                       /* SUB ADD CMP EOR, the A forms, SUBX ADDX CMPM */
        if (ss == 3) return 2 + EA(op, (op & 0x0100) ? 4 : 2);
        if ((op >> 12) != 0xB && (op & 0x0130) == 0x0100) return 2;   /* SUBX, ADDX */
        return 2 + EA(op, size_ss[ss]);

    case 0xE: return ss == 3 ? 2 + EA(op, 2) : 2;       /* shifts on memory / on registers */
    }
    return 2;                                           /* line A, line F */
}
