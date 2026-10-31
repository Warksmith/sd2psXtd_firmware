#pragma once

#include <stdbool.h>
#include <stdint.h>

void ps2_memory_card_main(void);
void ps2_memory_card_enter(void);
void ps2_memory_card_exit(void);
void ps2_memory_card_unload(void);
bool ps2_memory_card_running(void);
/* When the console first looked for a card, in ms since power-on (0: not yet), and how
 * many times it found none because the card was hidden. */
void ps2_memory_card_polls(uint32_t *first_ms, uint32_t *ignored);