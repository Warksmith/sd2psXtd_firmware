#include <assert.h>
#include <stdio.h>
#include "usb_startup.h"
#include "config.h"
static unsigned buttons, samples, delays, recovery;
void input_task(void) { samples++; }
void sleep_ms(unsigned ms) { assert(ms == 1); delays++; }
int input_is_down_raw(int index) { assert(samples == 2 * DEBOUNCE_MS && samples == delays); return (buttons >> index) & 1; }
void reset_usb_boot(unsigned gpio, unsigned mask) { assert(gpio == 0 && mask == 0); recovery++; }
int main(void) {
    for (buttons = 0; buttons < 4; buttons++) {
        samples = delays = recovery = 0;
        assert(usb_startup_select_storage() == (buttons == 1));
        assert(recovery == ((buttons & 2) != 0));
    }
    puts("Startup debounce, normal boot, Button 1 Mass and Button 2 recovery priority passed");
}
