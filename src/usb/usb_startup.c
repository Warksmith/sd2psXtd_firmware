#include "usb_startup.h"
#include "config.h"
#include "input.h"
#include "pico/bootrom.h"
#include "pico/time.h"

bool usb_startup_select_storage(void) {
    for (int i = 0; i < 2 * DEBOUNCE_MS; ++i) {
        input_task();
        sleep_ms(1);
    }
    /* Firmware recovery takes priority even when both buttons are held. */
    if (input_is_down_raw(1)) {
        reset_usb_boot(0, 0);
        return false;
    }
    return input_is_down_raw(0) != 0;
}
