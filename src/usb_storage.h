#pragma once

/* Exposes the whole microSD card to the USB host as a mass storage device. Does not
   return: the device stays in this mode until it loses power. Must run instead of
   sd_init() and stdio_usb_init(), so the host is the only writer on the card. */
void usb_storage_run(void);
