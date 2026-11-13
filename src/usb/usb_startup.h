#pragma once
#include <stdbool.h>
/* Debounce startup buttons. Button 2 enters BOOTSEL; Button 1 requests Mass. */
bool usb_startup_select_storage(void);
