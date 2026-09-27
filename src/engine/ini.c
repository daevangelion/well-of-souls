#include "ini.h"
#include "text.h"
#include <string.h>
int ini_parse(Ini *ini, char *text)
{
    char *cursor = text, *line, *section = "";
    ini->count = 0;
    while ((line = text_next_line(&cursor))) {
        char *p, *q;
        for (p = line; *p; ++p) {
            if (*p == ';' || (*p == '/' && p[1] == '/')) { *p = 0; break; }
        }
        line = text_trim(line);
        if (!*line) continue;
        if (*line == '[') {
            p = strchr(line + 1, ']');
            if (!p || *text_trim(p + 1)) return -1;
            *p = 0; section = text_trim(line + 1);
            if (!*section) return -1;
        } else {
            p = strchr(line, '=');
            if (!p || ini->count == INI_MAX_ENTRIES) return -1;
            *p++ = 0; q = text_trim(line);
            if (!*q) return -1;
            ini->entries[ini->count].section = section;
            ini->entries[ini->count].key = q;
            ini->entries[ini->count++].value = text_trim(p);
        }
    }
    return 0;
}
const char *ini_get(const Ini *ini, const char *section, const char *key, const char *fallback)
{
    size_t i = ini->count;
    while (i--) {
        const IniEntry *e = &ini->entries[i];
        if (!text_casecmp(e->section, section) && !text_casecmp(e->key, key)) return e->value;
    }
    return fallback;
}
