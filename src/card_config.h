#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_FOLDER_NAME_LENGTH      ( 32 )

void card_config_read_channel_name(const char* card_folder, const char* card_base, const char* channel_number, char* name, size_t name_max_len);
uint8_t card_config_get_max_channels(const char* card_folder, const char* card_base);
uint8_t card_config_get_ps2_cardsize(const char* card_folder, const char* card_base);
/* True for a folder card. StorageBackend in the card's ini decides; without it what is on
 * the SD card does, and only a card with nothing there follows NewCardBackend, which the
 * boot card ignores. image_path is where the card's image is or would go. It is borrowed
 * to look for the folder beside it and comes back unchanged. */
bool card_config_get_ps2_folder_backend(const char* card_folder, const char* card_base, char* image_path, bool boot_card);
/* True if the card's ini asks for FolderSnapshot=ON, which is off by default. */
bool card_config_get_ps2_folder_snapshot(const char* card_folder, const char* card_base);
/* Exact ID > longest trailing-star prefix > opt-in DefaultCard, within the current
 * console section. Clears the output when no mapping applies. */
void card_config_get_card_folder(const char* game_id, char* card_folder, size_t card_folder_max_len);
bool card_config_read_image(uint8_t buff[1032], const char* card_folder, const char* card_base, int chan_idx);
