/* Musashi 4.5's m68k_is_valid_instruction() for the generator (3.1 in mame-go
 * has no disassembler). The relative path makes m68kdasm.c find the 4.5
 * headers next to it, whatever the include path of the build is. */
#include "../musashi/m68kdasm.c"

extern unsigned int fuzz_read16(unsigned int a);
unsigned int m68k_read_disassembler_8(unsigned int a) { return fuzz_read16(a & ~1u) >> ((a & 1) ? 0 : 8) & 0xFF; }
unsigned int m68k_read_disassembler_16(unsigned int a) { return fuzz_read16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return fuzz_read16(a) << 16 | fuzz_read16(a + 2); }
