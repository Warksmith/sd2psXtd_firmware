#include "mca_bridge.h"
#include "mca_protocol.h"
#include "mca_page.h"
#include "settings.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

extern mca_bridge_t mca_bridge;
static uint64_t now;
static const uint8_t *sent;
static size_t sent_length;
static bool complete_on_usb_task, card_present;
static bool mca_enabled = true;
static unsigned mca_setting_updates;
static bool auth_valid = true, card_accessible = true;
static int card_mode = 1, card_variant;
static int usb_mode = USB_MODE_MCA;
static uint32_t card_capacity = 8u * 1024 * 1024;
uint64_t time_us_64(void) { return now; }
void sleep_ms(uint32_t milliseconds) { now += milliseconds * 1000; }
void tud_task_ext(uint32_t timeout_ms, bool in_isr) {
    (void)timeout_ms; (void)in_isr;
    if (complete_on_usb_task) {
        complete_on_usb_task = false;
        mca_protocol_callbacks()->in_complete(true, sent_length);
    }
}
int settings_get_mode(bool temporary) { (void)temporary; return card_mode; }
int settings_get_usb_mode(bool current) {
    return current ? usb_mode : USB_MODE_MCA;
}
bool settings_get_mca_enabled(void) { return mca_enabled; }
void settings_set_mca_enabled(bool enabled) {
    mca_setting_updates++;
    mca_enabled = enabled;
}
int settings_get_ps2_variant(void) { return card_variant; }
bool ps2_cardman_is_accessible(void) { return card_accessible; }
bool ps2_cardman_is_idle(void) { return true; }
uint32_t ps2_cardman_get_card_size(void) { return card_capacity; }
bool ps2_mc_auth_isValid(void) { return auth_valid; }
void mca_device_set_presence(bool present) { card_present = present; }
void mca_device_task(void) {}
void mca_device_receive(void) {}
bool mca_device_profile_is_mca(void) { return true; }
bool mca_device_send(const uint8_t *data, size_t length) {
    assert(((uintptr_t)data & 3) == 0);
    sent = data;
    sent_length = length;
    return true;
}


static void ack(void) {
    assert(mca_bridge_state(&mca_bridge) == MCA_BRIDGE_SENDING);
    mca_protocol_callbacks()->in_complete(true, sent_length);
    assert(mca_bridge_state(&mca_bridge) == MCA_BRIDGE_IDLE);
}
static void submit(const uint8_t *bytes, size_t length) {
    const mca_device_callbacks_t *cb = mca_protocol_callbacks();
    sent_length = 0;
    for (size_t pos = 0; pos < length;) {
        size_t n = length - pos; if (n > 64) n = 64;
        cb->out_received(bytes + pos, (uint16_t)n); pos += n;
        mca_protocol_task();
    }
}
int main(void) {
    const mca_device_callbacks_t *cb = mca_protocol_callbacks();
    mca_protocol_init(); mca_protocol_set_ready(true);
    uint8_t request[1028] = {0xaa, 0x42, 0, 4};
    for (size_t i = 0; i < 1024; ++i) request[4+i] = (uint8_t)(17*i);
    submit(request, sizeof(request));
    assert(mca_bridge_begin(&mca_bridge));
    for (size_t i = 0; i < 1024; ++i) {
        uint8_t byte; assert(mca_bridge_receive(&mca_bridge, &byte) && byte == request[4+i]);
        mca_bridge_respond(&mca_bridge, byte ^ 0x5a);
    }
    mca_bridge_finish(&mca_bridge, true); mca_protocol_task();
    assert(sent_length == 1028 && sent[0] == 0x55 && sent[1] == 0x5a && sent[2] == 0 && sent[3] == 4);
    for (size_t i = 0; i < 1024; ++i) assert(sent[4+i] == (uint8_t)(request[4+i] ^ 0x5a));
    complete_on_usb_task = true; mca_protocol_drain_reply(500);
    assert(mca_bridge_state(&mca_bridge) == MCA_BRIDGE_IDLE);
    const uint8_t raw[] = {0xaa, 0x42, 3, 0, 0x81, 0x11, 0};
    const uint8_t detect[] = {0xaa, 0x40};
    for (int mass = 0; mass < 2; ++mass) {
        submit(raw, sizeof(raw)); assert(mca_bridge_begin(&mca_bridge));
        if (mass) usb_mode = USB_MODE_MASS; else mca_protocol_set_ready(false);
        mca_bridge_respond(&mca_bridge, 0xff); mca_bridge_finish(&mca_bridge, true);
        mca_protocol_task(); assert(sent_length == 5 && sent[4] == 0xff); ack();
        submit(detect, sizeof(detect)); assert(sent_length == 2 && sent[1] == 0); ack();
        submit(raw, sizeof(raw)); assert(sent_length == 2 && sent[1] == 0xaf); ack();
        usb_mode = USB_MODE_MCA; mca_protocol_set_ready(true);
    }
    /* Reset does not release a buffer still owned by the worker. */
    submit(raw, sizeof(raw)); assert(mca_bridge_begin(&mca_bridge));
    cb->bus_reset(); assert(mca_bridge_state(&mca_bridge) == MCA_BRIDGE_ACTIVE);
    mca_bridge_respond(&mca_bridge, 0xff); mca_bridge_finish(&mca_bridge, true);
    mca_protocol_task(); assert(mca_bridge_state(&mca_bridge) == MCA_BRIDGE_IDLE && sent_length == 0);
    /* Unclaimed and active timeouts follow different ownership paths. */
    submit(raw, sizeof(raw)); now += 1000001; mca_protocol_task();
    assert(sent_length == 2 && sent[1] == 0xaf); ack();
    submit(raw, sizeof(raw)); assert(mca_bridge_begin(&mca_bridge));
    now += 1000001; mca_protocol_task(); assert(mca_bridge_cancelled(&mca_bridge));
    uint8_t byte; assert(!mca_bridge_receive(&mca_bridge, &byte));
    mca_bridge_finish(&mca_bridge, true); mca_protocol_task();
    assert(sent_length == 2 && sent[1] == 0xaf); ack();
    const uint8_t page_request[] = {0xaa, 0x52, 3, 42, 0, 0, 0, 0x55, 0x2b};
    for (unsigned test = 0; test < 5; ++test) {
        auth_valid = test != 0; card_accessible = test != 1;
        card_mode = test == 2 ? 0 : 1; card_variant = test == 3 ? 1 : 0;
        card_capacity = test == 4 ? 42u * 512 : 8u * 1024 * 1024;
        submit(page_request, sizeof(page_request));
        assert(sent_length == 2 && sent[1] == 0xaf); ack();
    }
    critical_section_deinit(&mca_bridge.lock);
    puts("MCA packet framing, ownership, Mass handover, reset, timeout and page admission passed");
}
