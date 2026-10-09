/* Host stub: the gpSP sources use nothing of retro-go but this clock. */
#ifndef GBAHOST_RG_SYSTEM_H
#define GBAHOST_RG_SYSTEM_H
#include <stdint.h>
#include <time.h>
static inline int64_t rg_system_timer(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
#endif
