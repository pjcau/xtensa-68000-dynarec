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

/* glue_musashi.c */
bool glue_jit_init(bool (*is_code)(uint32_t, int), uint16_t (*read_code16)(uint32_t));
int glue_jit_execute(int num_cycles);

#endif
