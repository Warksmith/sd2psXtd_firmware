#pragma once
#include <stdbool.h>
void multicore_launch_core1(void (*entry)(void));
void multicore_reset_core1(void);
