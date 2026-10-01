#ifndef FUZZ_H
#define FUZZ_H

#include <stdint.h>
#include <stdbool.h>

void *fuzz_alloc(int size);                 /* from the host program (PSRAM on the ESP32-S3) */
extern bool fuzz_translate;
extern int fuzz_native_bias;
extern int fuzz_form;
#define FUZZ_FORMS 23
int fuzz_init(void);
int fuzz_seed(uint32_t seed, int slices, bool verbose);

/* glue_musashi.c (Musashi 4.5) or components/m68kjit/glue/glue_musashi31.c (mame-go) */
#ifdef FUZZ_MUSASHI31
bool glue31_jit_init(bool (*is_code)(uint32_t, int), uint16_t (*read_code16)(uint32_t), uint32_t code_size, int hot_threshold);
int glue31_jit_execute(int num_cycles);
extern int glue_hot_threshold;
#define glue_jit_init(c, r) glue31_jit_init(c, r, 0, glue_hot_threshold)
#define glue_jit_execute glue31_jit_execute
#else
extern int glue_hot_threshold;
bool glue_jit_init(bool (*is_code)(uint32_t, int), uint16_t (*read_code16)(uint32_t));
int glue_jit_execute(int num_cycles);
#endif

#endif
