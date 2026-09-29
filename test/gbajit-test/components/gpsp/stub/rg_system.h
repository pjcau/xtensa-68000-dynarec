/* gbajit-test: the gpSP core only needs IRAM_ATTR (and rg_system_timer for GBAPROF) from retro-go */
#pragma once
#include "esp_attr.h"
#include "esp_timer.h"
static inline int64_t rg_system_timer(void) { return esp_timer_get_time(); }
