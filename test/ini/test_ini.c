#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ini.h"
static const char *data;
static size_t pos;
static int hits;
int sd_read(int fd, void *buf, size_t count) {
    (void)fd;
    size_t left = strlen(data) - pos;
    if (count > left) count = left;
    memcpy(buf, data + pos, count); pos += count;
    return (int)count;
}
static int handler(void *user, const char *section, const char *name, const char *value) {
    (void)user;
    assert(strcmp(section, "Settings") == 0);
    assert(strcmp(name, "MaxChannels") == 0);
    assert(strcmp(value, "4") == 0);
    hits++;
    return 1;
}
int main(void) {
    const char *cases[] = {"[Settings]\nMaxChannels=4", "[Settings]\nMaxChannels=4\n", "\xEF\xBB\xBF[Settings]\r\nMaxChannels=4", "[Settings]\r\nMaxChannels=4\r\n"};
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        data = cases[i]; pos = 0; hits = 0;
        assert(ini_parse_sd_file(1, handler, NULL) == 0);
        assert(hits == 1);
    }
    data = ""; pos = 0; hits = 0;
    assert(ini_parse_sd_file(1, handler, NULL) == 0 && hits == 0);
    puts("PASS: INI final-line, newline, BOM, CRLF and empty-file regression cases");
}
