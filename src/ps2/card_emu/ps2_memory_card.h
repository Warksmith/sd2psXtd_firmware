#pragma once

#include <stdbool.h>
#include <stdint.h>


/**
 * @brief Main function for the memory card backend
 */
void ps2_memory_card_main(void);

/**
 * @brief Enter the memory card backend
 *       This is usually called when initializing the card
 */
void ps2_memory_card_enter(void);

/**
 * @brief Exit the memory card backend
 *       This is usually called when switching the card
 */
void ps2_memory_card_exit(void);

/* First console poll in ms since boot, and polls ignored while hidden. */
void ps2_memory_card_polls(uint32_t *first_ms, uint32_t *ignored);
