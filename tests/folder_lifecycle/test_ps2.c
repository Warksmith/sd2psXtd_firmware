#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "settings.h"
#include "ps2.h"
static bool backend_running = true, idle = true, can_unload = true, history_pending;
static int mode = MODE_PS2, variant = PS2_VARIANT_RETAIL, usb_mode = USB_MODE_UART;
static unsigned stage, attempts, retries, serviced;
bool ps2_card_backend_task(void) { return backend_running; }
void ps2_mmceman_task(void) {}
void ps2_cardman_task(void) { serviced++; }
void ps2_mmceman_fs_run(void) {}
bool ps2_cardman_is_accessible(void) { return true; }
bool ps2_cardman_is_idle(void) { return idle; }
bool ps2_cardman_can_unload(void) { return can_unload; }
void ps2_history_tracker_task(void) {}
bool ps2_history_tracker_needs_refresh(void) { return history_pending; }
void ps2_mc_data_interface_task(void) {}
bool ps2_mc_auth_keyStoreResetRequired(void) { return false; }
void ps2_mc_auth_keyStoreResetAck(void) {}
bool ps2_mc_auth_isValid(void) { return false; }
void keystore_reset(void) {}
void keystore_confirm(void) {}
int settings_get_mode(bool current) { (void)current; return mode; }
int settings_get_ps2_variant(void) { return variant; }
int settings_get_usb_mode(bool current) { (void)current; return usb_mode; }
void ps2_memory_card_exit(void) { assert(stage == 0); stage = 1; }
bool ps2_mc_data_interface_write_occured(void) { return false; }
void multicore_reset_core1(void) { assert(stage == 1); stage = 2; }
void ps2_mc_data_interface_flush(void) { assert(stage == 2); stage = 3; }
bool ps2_cardman_prepare_close(void) { assert(stage == 3); return ++attempts == 3; }
void sleep_ms(uint32_t ms) { assert(ms == 10 && stage == 3); retries++; }
void ps2_cardman_close(void) { assert(stage == 3 && attempts == 3); stage = 4; }
int main(void) {
    assert(ps2_task());
    for (unsigned request = 0; request < 4; request++) {
        mode = MODE_PS2; variant = PS2_VARIANT_RETAIL;
        usb_mode = USB_MODE_UART; backend_running = true;
        if (request == 0) mode = MODE_PS1;
        if (request == 1) variant = PS2_VARIANT_COH;
        if (request == 2) usb_mode = USB_MODE_MASS;
        if (request == 3) backend_running = false;
        can_unload = false; assert(ps2_task());
        can_unload = true; idle = false; assert(ps2_task());
        idle = true; history_pending = true; assert(ps2_task());
        history_pending = false; assert(!ps2_task());
    }
    unsigned before = serviced;
    ps2_deinit();
    assert(stage == 4 && retries == 2 && serviced == before + 2);
    puts("PS2 mode/USB/backend transitions drain saves; failed final flushes retain the card");
}
