#include "card_config.h"
#include "settings.h"
#include "sd.h"
#include "debug.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ini.h"


#if LOG_LEVEL_CARD_CONF == 0
    #define log(x...)
#else
    #define log(level, fmt, x...) LOG_PRINT(LOG_LEVEL_CARD_CONF, level, fmt, ##x)
#endif

#define MAX_CFG_PATH_LENGTH         (64)
#define CUSTOM_CARDS_CONFIG_PATH    (".sd2psx/Game2Folder.ini")

typedef enum {
    BACKEND_UNSET,
    BACKEND_IMAGE,
    BACKEND_FOLDER,
} card_backend_t;

typedef struct {
    const char *channel_number;
    char *channel_name;
    size_t channel_name_max_len;
    uint8_t card_size;
    uint8_t max_channels;
    card_backend_t backend;
    bool folder_snapshot;
} parse_card_config_t;

typedef struct {
    const char *game_id;
    const char *mode;
    char *card_folder;
    size_t card_folder_max_len;
} parse_custom_card_folder_t;

static int parse_custom_card_folder(void *user, const char *section, const char *name, const char *value) {
    parse_custom_card_folder_t *ctx = user;

    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH(ctx->mode, ctx->game_id)) {
        if (ctx->card_folder != NULL && ctx->card_folder_max_len > 0) {
            strlcpy(ctx->card_folder, value, ctx->card_folder_max_len);
        }
    }
    #undef MATCH

    log(LOG_TRACE, "s=%s n=%s card_folder=%s\n", section, name, ctx->card_folder);

    return 1;
}

static int parse_card_configuration(void *user, const char *section, const char *name, const char *value) {
    parse_card_config_t *ctx = user;

    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH("ChannelName", ctx->channel_number)) {
        if (ctx->channel_name != NULL && ctx->channel_name_max_len > 0) {
            strlcpy(ctx->channel_name, value, ctx->channel_name_max_len);
        }
    } else if (MATCH("Settings", "CardSize")) {
        int size = atoi(value);
        switch (size) {
            case 1:
            case 2:
            case 4:
            case 8:
            case 16:
            case 32:
            case 64:
            case 128:
                ctx->card_size = size;
                break;
            default:
                break;
        }
    } else if (MATCH("Settings", "MaxChannels")) {
        int max_channels = atoi(value);
        if (max_channels > 0 && max_channels <= UINT8_MAX) {
            ctx->max_channels = max_channels;
        }
    } else if (MATCH("Settings", "StorageBackend")) {
        if (strcasecmp(value, "FOLDER") == 0) {
            ctx->backend = BACKEND_FOLDER;
        } else if (strcasecmp(value, "IMAGE") == 0) {
            ctx->backend = BACKEND_IMAGE;
        } else {
            log(LOG_ERROR, "Ignoring unknown StorageBackend=%s\n", value);
        }
    } else if (MATCH("Settings", "FolderSnapshot")) {
        if (strcasecmp(value, "ON") == 0) {
            ctx->folder_snapshot = true;
        } else if (strcasecmp(value, "OFF") == 0) {
            ctx->folder_snapshot = false;
        } else {
            log(LOG_ERROR, "Ignoring unknown FolderSnapshot=%s\n", value);
        }
    }
    #undef MATCH

    return 1;
}


static void card_config_get_image_name(const char* card_folder, const char* card_base, char* image_path) {
    if (settings_get_mode(true) == MODE_PS1) {
        snprintf(image_path, MAX_CFG_PATH_LENGTH, "MemoryCards/PS1/%s/%s.bin", card_folder, card_base);
    } else {
        switch (settings_get_ps2_variant()) {
            case PS2_VARIANT_PROTO:
                snprintf(image_path, MAX_CFG_PATH_LENGTH, "MemoryCards/PROT/%s/%s.bin", card_folder, card_base);
            break;
            case PS2_VARIANT_COH:
                snprintf(image_path, MAX_CFG_PATH_LENGTH, "MemoryCards/COH/%s/%s.bin", card_folder, card_base);
            break;
            case PS2_VARIANT_SC2:
                snprintf(image_path, MAX_CFG_PATH_LENGTH, "MemoryCards/SC2/%s/%s.bin", card_folder, card_base);
            break;
            case PS2_VARIANT_RETAIL:
            default:
                snprintf(image_path, MAX_CFG_PATH_LENGTH, "MemoryCards/PS2/%s/%s.bin", card_folder, card_base);
            break;
        }
    }
    log(LOG_TRACE, "image=%s\n", image_path);

}

static void card_config_get_ini_name(const char* card_folder, const char* card_base, char* config_path) {
    if (settings_get_mode(true) == MODE_PS1) {
        snprintf(config_path, MAX_CFG_PATH_LENGTH, "MemoryCards/PS1/%s/%s.ini", card_folder, card_base);
    } else {
        switch (settings_get_ps2_variant()) {
            case PS2_VARIANT_PROTO:
                snprintf(config_path, MAX_CFG_PATH_LENGTH, "MemoryCards/PROT/%s/%s.ini", card_folder, card_base);
            break;
            case PS2_VARIANT_COH:
                snprintf(config_path, MAX_CFG_PATH_LENGTH, "MemoryCards/COH/%s/%s.ini", card_folder, card_base);
            break;
            case PS2_VARIANT_SC2:
                snprintf(config_path, MAX_CFG_PATH_LENGTH, "MemoryCards/SC2/%s/%s.ini", card_folder, card_base);
            break;
            case PS2_VARIANT_RETAIL:
            default:
                snprintf(config_path, MAX_CFG_PATH_LENGTH, "MemoryCards/PS2/%s/%s.ini", card_folder, card_base);
            break;
        }
    }
    log(LOG_TRACE, "config_path=%s\n", config_path);

}

bool card_config_read_image(uint8_t buff[1032], const char* card_folder, const char* card_base, int chan_idx) {
    char image_path[64];
    char full_cardname[16];
    int fd;

    snprintf(full_cardname, 16, "%s-%i", card_base, chan_idx);

    card_config_get_image_name(card_folder, full_cardname, image_path);

    if (!sd_exists(image_path)) {
        card_config_get_image_name(card_folder, card_base, image_path);
    }

    fd = sd_open(image_path, O_RDONLY);
    if (fd >= 0) {
        sd_read(fd, buff, 1032);
        sd_close(fd);
        log(LOG_TRACE, "Read image %s\n", image_path);
        return true;
    } else {
        memset(buff, 0, 1032);
        return false;
    }
}

void card_config_read_channel_name(const char* card_folder, const char* card_base, const char* channel_number, char* name, size_t name_max_len) {
    char config_path[64];
    int fd;

    card_config_get_ini_name(card_folder, card_base, config_path);

    fd = sd_open(config_path, O_RDONLY);
    if (fd >= 0) {
        parse_card_config_t ctx = {
            .channel_number = channel_number,
            .channel_name = name,
            .channel_name_max_len = name_max_len,
            .card_size = 0,
            .max_channels = 8
        };
        ini_parse_sd_file(fd, parse_card_configuration, &ctx);
        sd_close(fd);
    }
}

uint8_t card_config_get_ps2_cardsize(const char* card_folder, const char* card_base) {
    char config_path[64];
    int fd;
    parse_card_config_t ctx = {
        .channel_number = NULL,
        .channel_name = NULL,
        .channel_name_max_len = 0,
        .card_size = 0,
        .max_channels = 8
    };

    card_config_get_ini_name(card_folder, card_base, config_path);

    fd = sd_open(config_path, O_RDONLY);
    if (fd >= 0) {
        ini_parse_sd_file(fd, parse_card_configuration, &ctx);
        sd_close(fd);
    }
    return ctx.card_size;
}

static bool folder_beside_image(char* image_path) {
    char *extension = strrchr(image_path, '.');
    bool found;

    if (extension == NULL)
        return false;

    strcpy(extension, ".folder");
    found = sd_exists(image_path);
    strcpy(extension, ".mcd");
    return found;
}

bool card_config_get_ps2_folder_backend(const char* card_folder, const char* card_base, char* image_path, bool boot_card) {
    char config_path[MAX_CFG_PATH_LENGTH];
    int fd;
    parse_card_config_t ctx = {
        .channel_number = NULL,
        .channel_name = NULL,
        .channel_name_max_len = 0,
        .card_size = 0,
        .max_channels = 8,
        .backend = BACKEND_UNSET
    };

    card_config_get_ini_name(card_folder, card_base, config_path);

    fd = sd_open(config_path, O_RDONLY);
    if (fd >= 0) {
        ini_parse_sd_file(fd, parse_card_configuration, &ctx);
        sd_close(fd);
    }
    if (ctx.backend != BACKEND_UNSET)
        return ctx.backend == BACKEND_FOLDER;

    /* A card that already exists stays what it is, or its saves would vanish behind a
     * blank card of the other kind. */
    if (sd_exists(image_path))
        return false;
    if (folder_beside_image(image_path))
        return true;

    /* The boot card has to answer at power-on, which a folder card can miss. */
    return settings_get_ps2_new_card_folder() && !boot_card
           && settings_get_ps2_variant() == PS2_VARIANT_RETAIL
           && (ctx.card_size != 0 ? ctx.card_size : settings_get_ps2_cardsize()) == 8;
}

bool card_config_get_ps2_folder_snapshot(const char* card_folder, const char* card_base) {
    char config_path[MAX_CFG_PATH_LENGTH];
    int fd;
    parse_card_config_t ctx = {
        .channel_number = NULL,
        .channel_name = NULL,
        .channel_name_max_len = 0,
        .card_size = 0,
        .max_channels = 8,
        .folder_snapshot = false
    };

    card_config_get_ini_name(card_folder, card_base, config_path);

    fd = sd_open(config_path, O_RDONLY);
    if (fd >= 0) {
        ini_parse_sd_file(fd, parse_card_configuration, &ctx);
        sd_close(fd);
    }
    return ctx.folder_snapshot;
}

uint8_t card_config_get_max_channels(const char* card_folder, const char* card_base) {
    char config_path[MAX_CFG_PATH_LENGTH];
    int fd;
    parse_card_config_t ctx = {
        .channel_number = NULL,
        .channel_name = NULL,
        .channel_name_max_len = 0,
        .card_size = 0,
        .max_channels = 8
    };

    card_config_get_ini_name(card_folder, card_base, config_path);

    fd = sd_open(config_path, O_RDONLY);
    if (fd >= 0) {
        log(LOG_TRACE, "config_path=%s\n", config_path);
        ini_parse_sd_file(fd, parse_card_configuration, &ctx);
        sd_close(fd);
    }
    log(LOG_TRACE, "max_channels=%d\n", ctx.max_channels);
    return ctx.max_channels;
}


void card_config_get_card_folder(const char* game_id, char* card_folder, size_t card_folder_max_len) {
    char mode[9];
    parse_custom_card_folder_t ctx = {
        .game_id = game_id,
        .mode = mode,
        .card_folder = card_folder,
        .card_folder_max_len = card_folder_max_len
    };

    if (settings_get_mode(true) == MODE_PS1) {
        snprintf(mode, sizeof(mode), "PS1");
    } else {
        switch (settings_get_ps2_variant()) {
            case PS2_VARIANT_PROTO:
                snprintf(mode, sizeof(mode), "PROT");
            break;
            case PS2_VARIANT_COH:
                snprintf(mode, sizeof(mode), "COH");
            break;
            case PS2_VARIANT_SC2:
                snprintf(mode, sizeof(mode), "CONQUEST");
            break;
            case PS2_VARIANT_RETAIL:
            default:
                snprintf(mode, sizeof(mode), "PS2");
            break;
        }
    }
    int fd =  sd_open(CUSTOM_CARDS_CONFIG_PATH, O_RDONLY);
    log(LOG_TRACE, "Looking for game_id=%s mode=%s \n", game_id, ctx.mode);

    if (fd >= 0) {
        ini_parse_sd_file(fd, parse_custom_card_folder, &ctx);
        sd_close(fd);
    }

    log(LOG_TRACE, "found card_folder=%s\n", card_folder);
}
