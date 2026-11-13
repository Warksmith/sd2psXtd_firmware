#pragma once
#include <stdbool.h>

void usb_storage_init(void);
bool usb_storage_task(void);
void usb_storage_deinit(void);

typedef enum {
    USB_STORAGE_WAITING,
    USB_STORAGE_CONNECTED,
    USB_STORAGE_EJECTING,
    USB_STORAGE_EJECTED,
} usb_storage_status_t;
usb_storage_status_t usb_storage_get_status(void);
