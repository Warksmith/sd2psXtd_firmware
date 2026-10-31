#include "ps2_folder_name.h"

#include <string.h>

static const char hexdigits[] = "0123456789ABCDEF";

static char upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static size_t raw_len(const char raw[PFS_NAME_LEN]) {
    size_t len = 0;

    while (len < PFS_NAME_LEN && raw[len])
        len++;
    return len;
}

static bool equal_nocase(const char *a, size_t a_len, const char *b) {
    size_t b_len = strlen(b);

    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        if (upper(a[i]) != upper(b[i]))
            return false;
    }
    return true;
}

bool pfn_host_junk(const char *host) {
    size_t len = strlen(host);

    return equal_nocase(host, len, ".DS_Store") || equal_nocase(host, len, "Thumbs.db")
           || equal_nocase(host, len, "desktop.ini") || (len >= 2 && host[0] == '.' && host[1] == '_');
}

/* Names that must not reach a host with their literal spelling. */
static bool reserved_name(const char *name, size_t len) {
    static const char *const device[] = {"CON", "PRN", "AUX", "NUL"};
    char copy[PFS_NAME_LEN + 1];
    size_t stem = 0;

    while (stem < len && name[stem] != '.')
        stem++;

    for (size_t i = 0; i < sizeof(device) / sizeof(device[0]); i++) {
        if (equal_nocase(name, stem, device[i]))
            return true;
    }
    if (stem == 4 && name[3] >= '1' && name[3] <= '9'
        && (equal_nocase(name, 3, "COM") || equal_nocase(name, 3, "LPT")))
        return true;

    if (equal_nocase(name, len, ".ps2meta"))
        return true;

    memcpy(copy, name, len);
    copy[len] = 0;
    return pfn_host_junk(copy);
}

static bool must_escape(char c, bool strict) {
    unsigned char u = (unsigned char)c;

    if (u < 0x20 || u >= 0x7F)
        return true;
    if (strict && c >= 'a' && c <= 'z')
        return true;

    switch (c) {
        case '"': case '*': case '/': case ':': case '<':
        case '>': case '?': case '\\': case '|': case '%':
            return true;
        default:
            return false;
    }
}

void pfn_encode(const char raw[PFS_NAME_LEN], bool strict, char out[PFN_HOST_NAME_MAX]) {
    size_t len = raw_len(raw);
    bool reserved = reserved_name(raw, len);
    char *p = out;

    for (size_t i = 0; i < len; i++) {
        char c = raw[i];
        bool last = (i + 1 == len);

        if (must_escape(c, strict) || (i == 0 && (reserved || c == ' ')) || (last && (c == '.' || c == ' '))) {
            *p++ = '%';
            *p++ = hexdigits[(unsigned char)c >> 4];
            *p++ = hexdigits[(unsigned char)c & 0x0F];
        } else {
            *p++ = c;
        }
    }
    *p = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool pfn_valid_raw(const char raw[PFS_NAME_LEN]) {
    size_t len = raw_len(raw);

    if (len == 0)
        return false;
    if (raw[0] == '.' && (len == 1 || (len == 2 && raw[1] == '.')))
        return false;
    return true;
}

bool pfn_decode(const char *host, char raw[PFS_NAME_LEN], bool *strict) {
    static char check[PFN_HOST_NAME_MAX];
    size_t len = 0;

    memset(raw, 0, PFS_NAME_LEN);

    for (const char *p = host; *p; p++) {
        char c = *p;

        if (c == '%') {
            int hi = hexval(p[1]);
            int lo = (hi < 0) ? -1 : hexval(p[2]);
            if (lo < 0)
                return false;
            c = (char)((hi << 4) | lo);
            if (c == 0)
                return false;
            p += 2;
        }
        /* The last byte stays a terminator, as mcman requires. */
        if (len >= PFS_NAME_LEN - 1)
            return false;
        raw[len++] = c;
    }

    if (!pfn_valid_raw(raw))
        return false;

    pfn_encode(raw, false, check);
    if (strcmp(check, host) == 0) {
        *strict = false;
        return true;
    }
    pfn_encode(raw, true, check);
    if (strcmp(check, host) == 0) {
        *strict = true;
        return true;
    }
    return false;
}

bool pfn_has_lower(const char raw[PFS_NAME_LEN]) {
    for (size_t i = 0; i < PFS_NAME_LEN && raw[i]; i++) {
        if (raw[i] >= 'a' && raw[i] <= 'z')
            return true;
    }
    return false;
}

uint32_t pfn_fold_hash(const char raw[PFS_NAME_LEN]) {
    uint32_t hash = 0x811C9DC5U;

    for (size_t i = 0; i < PFS_NAME_LEN && raw[i]; i++) {
        hash ^= (uint8_t)upper(raw[i]);
        hash *= 0x01000193U;
    }
    return hash;
}

bool pfn_fold_equal(const char a[PFS_NAME_LEN], const char b[PFS_NAME_LEN]) {
    for (size_t i = 0; i < PFS_NAME_LEN; i++) {
        if (upper(a[i]) != upper(b[i]))
            return false;
        if (!a[i])
            return true;
    }
    return true;
}
