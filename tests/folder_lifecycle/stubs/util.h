#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
uint64_t RAM_time_us_64(void);
bool try_set_named_card_folder(const char *dir, int index, char *name, size_t size);
