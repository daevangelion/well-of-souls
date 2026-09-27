#ifndef WOS_INI_H
#define WOS_INI_H
#include <stddef.h>
#define INI_MAX_ENTRIES 2048
typedef struct { char *section, *key, *value; } IniEntry;
typedef struct { IniEntry entries[INI_MAX_ENTRIES]; size_t count; } Ini;
/* Mutates caller-owned text. Ini borrows it until the next parse. Returns 0 on success. */
int ini_parse(Ini *ini, char *text);
const char *ini_get(const Ini *ini, const char *section, const char *key, const char *fallback);
#endif
