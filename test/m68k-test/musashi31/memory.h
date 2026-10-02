/* stand-in for MAME's memory.h: the 24-bit bus of the fuzz */
#ifndef MEMORY_H
#define MEMORY_H
/* mame-go's placement attributes (IRAM code, internal-RAM data): nothing on the host */
#define MAMEGO_HOT
#define MAMEGO_DRAM
unsigned int cpu_readmem32(unsigned int a);
unsigned int cpu_readmem32_word(unsigned int a);
unsigned int cpu_readmem32_dword(unsigned int a);
void cpu_writemem32(unsigned int a, unsigned int v);
void cpu_writemem32_word(unsigned int a, unsigned int v);
void cpu_writemem32_dword(unsigned int a, unsigned int v);
#define cpu_readop_arg16(a) cpu_readmem32_word(a)
void fuzz31_change_pc(unsigned int pc);
#define change_pc32(pc) fuzz31_change_pc(pc)
#endif
