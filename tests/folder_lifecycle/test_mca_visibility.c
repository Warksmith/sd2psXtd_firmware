#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mca_bridge.h"
#include "ps2_card_backend_mca.h"
mca_bridge_t mca_bridge;
volatile bool ps2_card_active;
volatile int ps2_mc_exit_request;
volatile uint8_t mmceman_mcman_retry_counter, mmceman_tx_byte;
volatile bool mmceman_tx_queued;
void (*mmceman_callback)(void);
static bool accessible;
uint64_t time_us_64(void) { return 0; }
bool ps2_cardman_is_accessible(void) {
    if (!accessible) ps2_mc_exit_request = 1; /* end the worker after rejecting this request */
    return accessible;
}
int settings_get_mode(bool current) { (void)current; return 1; }
bool settings_get_mca_enabled(void) { return true; }
bool mca_protocol_idle(void) { return true; }
void mca_protocol_set_ready(bool ready) { (void)ready; }
void mca_protocol_switch_delay(uint32_t ms) { (void)ms; }
bool mca_page_active(void) { return false; }
bool mca_page_receive(uint8_t *byte) { (void)byte; return false; }
void mca_page_respond(uint8_t byte) { (void)byte; }
bool mca_page_execute(mca_bridge_t *bridge) { (void)bridge; assert(!"Hidden card reached a page handler"); return false; }
int main(void) {
    mca_bridge_init(&mca_bridge);
    const uint8_t commands[] = {0x42, 0x52, 0x57};
    for (size_t i = 0; i < sizeof(commands); ++i) {
        uint8_t byte;
        ps2_mc_exit_request = 0;
        memset(mca_bridge.input, 0, 8);
        mca_bridge.input[0] = 0xaa;
        mca_bridge.input[1] = commands[i];
        mca_bridge.input[4] = 0x81;
        assert(mca_bridge_submit(&mca_bridge, 8));
        assert(ps2_card_backend_mca.receiveFirst(&byte) == PS2_RECEIVE_EXIT);
        assert(mca_bridge_state(&mca_bridge) == MCA_BRIDGE_RESPONSE);
        assert(mca_bridge.output_length == 2 && mca_bridge.output[0] == 0x55 && mca_bridge.output[1] == 0xaf);
        assert(!ps2_card_active);
        mca_bridge_release(&mca_bridge);
    }
    accessible = true; ps2_mc_exit_request = 0;
    mca_bridge.input[1] = 0x42; mca_bridge.input[4] = 0x81;
    assert(mca_bridge_submit(&mca_bridge, 8));
    uint8_t byte;
    assert(ps2_card_backend_mca.receiveFirst(&byte) == PS2_RECEIVE_OK && byte == 0x81);
    ps2_card_backend_mca.finish();
    critical_section_deinit(&mca_bridge.lock);
    puts("Hidden folder cards reject MCA raw/read/write requests; visible cards remain usable");
}
