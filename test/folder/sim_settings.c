#include <stddef.h>

#include "settings.h"
#include "sim.h"

bool sim_new_card_folder;
uint8_t sim_ps2_cardsize = 8;
int sim_ps2_variant = PS2_VARIANT_RETAIL;

int settings_get_mode(bool current) {
    (void)current;
    return MODE_PS2;
}

int settings_get_ps2_variant(void) {
    return sim_ps2_variant;
}

uint8_t settings_get_ps2_cardsize(void) {
    return sim_ps2_cardsize;
}

bool settings_get_ps2_new_card_folder(void) {
    return sim_new_card_folder;
}

/* The firmware's C library has this and not every host's does. */
size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t len = 0;

    while (src[len])
        len++;
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        for (size_t i = 0; i < n; i++)
            dst[i] = src[i];
        dst[n] = 0;
    }
    return len;
}
