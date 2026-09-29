/* thumb.h - JIT step 4: a small Thumb translator and its reference (thumb.c) */
#ifndef THUMB_H
#define THUMB_H
#include <stdint.h>
#include <stdbool.h>
#include "xjit_block.h"

typedef struct { uint32_t r[16]; uint32_t n, z, c, v; } thumb_state_t;

void thumb_ref(thumb_state_t *s, const uint16_t *code, int count);
bool thumb_translate(xj_block_t *b, const uint16_t *code, int count);
uint16_t thumb_random_op(void);
void thumb_random_state(thumb_state_t *s);
#endif
