#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pico/time.h"

/* Exercise the real card manager's private lifecycle states and the wire-level
 * GET_STATUS handler. Unrelated hardware paths are removed by section GC. */
#include "../../src/ps2/ps2_cardman.c"
#undef log
#include "../../src/ps2/mmceman/ps2_mmceman_commands.c"

static bool folder_active;
static bool sdmode;
static uint8_t response[8];
static size_t response_size;
uint8_t term = 0xff;
volatile uint8_t mmceman_cmd;

bool ps2_folder_card_active(void) { return folder_active; }
bool ps2_mc_data_interface_get_sdmode(void) { return sdmode; }
static uint8_t recv_byte(uint8_t *byte) { *byte = 0; return PS2_RECEIVE_OK; }
static void respond_byte(uint8_t byte) { assert(response_size < sizeof(response)); response[response_size++] = byte; }
static ps2_memory_card_backend_t backend = {.receive = recv_byte, .respond = respond_byte};
ps2_memory_card_backend_t *ps2_card_backend = &backend;

static void expect_status(bool ready) {
    response_size = 0;
    ps2_mmceman_cmd_get_status();
    /* Identifier ACK is sent by mc_main_loop; this handler supplies reserved,
     * error, busy, terminator. */
    assert(response_size == 4);
    assert(response[0] == 0 && response[1] == 0 && response[3] == term);
    assert(response[2] == (ready ? 0 : 1));
}

int main(void) {
    folder_active = true;
    cardman_operation = CARDMAN_IDLE;
    expect_status(true);

    /* IGR must not see the old, idle card as ready before core 0 consumes BOOT. */
    mmceman_cmd = MMCEMAN_SWITCH_BOOTCARD;
    expect_status(false);
    needs_update = true;
    mmceman_cmd = 0;
    expect_status(false);

    needs_update = false;
    folder_hidden = true;
    cardman_operation = CARDMAN_FOLDER_LOAD;
    expect_status(false);
    /* Directory publication enables demand reads, not core-0 synchronous I/O. */
    cardman_operation = CARDMAN_FOLDER_FILL;
    folder_hidden = false;
    expect_status(true);
    assert(!ps2_cardman_is_idle());
    /* A published snapshot can likewise serve missing pages on demand. */
    cardman_operation = CARDMAN_OPEN;
    expect_status(true);
    assert(!ps2_cardman_is_idle());

    /* Failed/withdrawn cards never report ready, even once background work stops. */
    folder_hidden = true;
    cardman_operation = CARDMAN_IDLE;
    expect_status(false);
    cardman_operation = CARDMAN_UNKNOWN;
    folder_hidden = false;
    expect_status(false);

    /* Preserve image-card load/create behavior with and without PSRAM SD mode. */
    folder_active = false;
    for (unsigned mode = 0; mode < 2; ++mode) {
        sdmode = mode;
        cardman_operation = CARDMAN_OPEN;
        expect_status(false);
        cardman_operation = CARDMAN_CREATE;
        expect_status(false);
        cardman_operation = CARDMAN_IDLE;
        expect_status(true);
    }
    puts("MMCE readiness: queued switches stay busy; published folder/snapshot reads are ready; internal idle and image behavior retained");
}
