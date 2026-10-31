#pragma once

#include <stddef.h>
#include <stdint.h>

/* Generate the 512-byte page at byte offset `pos` of a freshly formatted card. */
void ps2_card_format_page(uint32_t card_size, size_t pos, void *buf);
