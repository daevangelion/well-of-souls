#include "text.h"
#include "../platform/platform.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#define TEXT_FILE_MAX (64u * 1024u * 1024u)
char *text_read_file(const char *path, size_t *size)
{
    FILE *f = plat_fopen(path, "rb");
    long n;
    char *s;
    if (size) *size = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 ||
        (unsigned long)n > TEXT_FILE_MAX || fseek(f, 0, SEEK_SET)) {
        fclose(f); return NULL;
    }
    s = malloc((size_t)n + 1);
    if (!s) { fclose(f); return NULL; }
    if (fread(s, 1, (size_t)n, f) != (size_t)n || ferror(f)) {
        free(s); fclose(f); return NULL;
    }
    fclose(f); s[n] = 0;
    if (size) *size = (size_t)n;
    return s;
}
char *text_trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s)) ++s;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) --e;
    *e = 0; return s;
}
int text_casecmp(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
char *text_next_line(char **cursor)
{
    char *s = *cursor, *e;
    if (!s || !*s) return NULL;
    e = s + strcspn(s, "\r\n");
    if (*e) {
        char c = *e; *e++ = 0;
        if (c == '\r' && *e == '\n') ++e;
        *cursor = e;
    } else *cursor = NULL;
    return s;
}
char *text_next_field(char **cursor)
{
    char *s = *cursor, *e;
    if (!s) return NULL;
    e = s + strcspn(s, ",\t");
    if (*e) { *e++ = 0; *cursor = e; } else *cursor = NULL;
    return text_trim(s);
}
