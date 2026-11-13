#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "settings.h"
#include "card_config.h"
static const char *data;
static size_t pos;
static int checks;
static int sim_mode = MODE_PS2, sim_ps2_variant = PS2_VARIANT_RETAIL;
#define CHECK(x) do { checks++; assert(x); } while (0)
uint8_t settings_get_ps1_maxchannels(void) { return 8; }
uint8_t settings_get_ps2_maxchannels(void) { return 8; }
int settings_get_mode(bool current) { (void)current; return sim_mode; }
int settings_get_ps2_variant(void) { return sim_ps2_variant; }
static void sim_sd_mkdirs(const char *p) { (void)p; }
static void sim_sd_put(const char *p, const char *s, size_t len) { (void)p; (void)len; data = s; }
int sd_open(const char *p, int flags) { (void)p; (void)flags; pos = 0; return data ? 1 : -1; }
int sd_close(int fd) { (void)fd; return 0; }
int sd_read(int fd, void *buf, size_t count) {
    (void)fd; size_t left = strlen(data) - pos;
    if (count > left) count = left;
    memcpy(buf, data + pos, count); pos += count; return (int)count;
}
static void check_mapping(const char *ini, const char *id, const char *expected) {
    char folder[MAX_FOLDER_NAME_LENGTH] = "stale previous card";
    sim_sd_mkdirs(".sd2psx");
    sim_sd_put(".sd2psx/Game2Folder.ini", ini, strlen(ini));
    card_config_get_card_folder(id, folder, sizeof(folder));
    CHECK(strcmp(folder, expected) == 0);
}

static void test_config_game2folder(void) {
    data = NULL;
    sim_mode = MODE_PS2;
    sim_ps2_variant = PS2_VARIANT_RETAIL;
    char folder[32] = "stale";
    card_config_get_card_folder("SLUS-12345", folder, sizeof(folder));
    CHECK(folder[0] == '\0'); /* Missing file clears previous selection. */
    check_mapping("[PS2]\nSLUS-12345=Exact\nSLUS-123**=Narrow\nSLUS-*=Wide\n*=All",
                  "SLUS-12345", "Exact");
    check_mapping("[PS2]\nSLUS-*=Wide\nSLUS-123**=Narrow", "SLUS-12399", "Narrow");
    check_mapping("[PS2]\nSLUS-123**=Narrow\nSLUS-*=Wide", "SLUS-12399", "Narrow");
    check_mapping("[PS2]\nSLUS-*=Wide\nSLUS-12345=Exact", "SLUS-12345", "Exact");
    check_mapping("[PS2]\nSLUS-123*=First\nSLUS-123**=Last", "SLUS-12345", "Last");
    check_mapping("[PS2]\nSLUS-12345=First\nSLUS-12345=Last", "SLUS-12345", "Last");
    check_mapping("[PS2]\nSLUS-*45=Invalid\nSLUS-123?" "?=Invalid", "SLUS-12345", "");
    check_mapping("[PS2]\nSLUS-123*=No", "SLUS-12", "");
    check_mapping("[PS2]\nSLUS-*=No", "slus-12345", "");
    check_mapping("[PS2]\nDefaultCard=Shared", "SLES-12345", "");
    check_mapping("[PS2]\nUseDefaultCard=OFF\nDefaultCard=Shared", "SLES-12345", "");
    check_mapping("[PS2]\nUseDefaultCard=ON", "SLES-12345", "");
    check_mapping("[PS2]\nUseDefaultCard=ON\nDefaultCard=", "SLES-12345", "");
    check_mapping("\xEF\xBB\xBF[PS2]\r\nDefaultCard=Shared\r\nUseDefaultCard=on", "SLES-12345", "Shared");
    check_mapping("[PS2]\nUseDefaultCard=ON\nDefaultCard=Shared\nSLUS-*=Wide", "SLUS-12345", "Wide");
    check_mapping("[PS2]\nSLUS-12345=\nUseDefaultCard=ON\nDefaultCard=Shared", "SLUS-12345", "");
    check_mapping("[PS2]\nUseDefaultCard=ON\nUseDefaultCard=TYPO\nDefaultCard=Shared", "SLES-12345", "");
    check_mapping("[PS1]\nUseDefaultCard=ON\nDefaultCard=Other\nSLUS-*=Other", "SLUS-12345", "");
    check_mapping("[PS2]\nUseDefaultCard=ON\nDefaultCard=Shared\n*=All", "", "");
    sim_mode = MODE_PS1;
    check_mapping("[PS2]\nSLUS-*=Other\n[PS1]\nSLUS-*=PS1Card", "SLUS-12345", "PS1Card");
    sim_mode = MODE_PS2;
    const int variants[] = {PS2_VARIANT_PROTO, PS2_VARIANT_COH, PS2_VARIANT_SC2};
    const char *expected[] = {"Proto", "Coh", "Conquest"};
    for (size_t i = 0; i < 3; i++) {
        sim_ps2_variant = variants[i];
        check_mapping("[PROT]\nUseDefaultCard=ON\nDefaultCard=Proto\n[COH]\nUseDefaultCard=ON\nDefaultCard=Coh\n[CONQUEST]\nUseDefaultCard=ON\nDefaultCard=Conquest", "SLUS-12345", expected[i]);
    }
    sim_ps2_variant = PS2_VARIANT_RETAIL;
    check_mapping("[PS2]\n*=All", "SLUS-12345", "All");
    char tiny[3] = "xx";
    card_config_get_card_folder("SLUS-12345", tiny, sizeof(tiny));
    CHECK(strcmp(tiny, "Al") == 0);
    card_config_get_card_folder("SLUS-12345", tiny, 0);
    CHECK(strcmp(tiny, "Al") == 0);
    card_config_get_card_folder("SLUS-12345", NULL, 0);
    card_config_get_card_folder(NULL, tiny, sizeof(tiny));
    CHECK(tiny[0] == '\0');
}

int main(void) { test_config_game2folder(); printf("PASS: %d Game2Folder checks\n", checks); }
