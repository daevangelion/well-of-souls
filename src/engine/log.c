#include "log.h"
#include "../platform/platform.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#define LOG_NAMES_MAX 1024
#define LOG_NAME_MAX 128
static FILE *stream;
static struct { char name[LOG_NAME_MAX]; uint64_t serial; } events[LOG_NAMES_MAX];
static size_t count;
static uint64_t serial;
int wos_log_open(const char *path)
{
    wos_log_close(); count = 0; serial = 0;
    stream = path ? plat_fopen(path, "w") : stdout;
    return stream ? 0 : -1;
}
void wos_log_close(void)
{
    if (stream && stream != stdout) fclose(stream);
    stream = NULL;
}
void wos_log_event(const char *name, const char *fmt, ...)
{
    size_t i;
    va_list ap;
    if (!stream) stream = stdout;
    fprintf(stream, "EVT %s", name);
    if (fmt && *fmt) {
        fputc(' ', stream); va_start(ap, fmt); vfprintf(stream, fmt, ap); va_end(ap);
    }
    fputc('\n', stream); fflush(stream);
    for (i = 0; i < count; ++i) if (!strcmp(events[i].name, name)) break;
    if (i == count) {
        if (count == LOG_NAMES_MAX || strlen(name) >= LOG_NAME_MAX) {
            fputs("log event registry exhausted\n", stderr); abort();
        }
        memcpy(events[i].name, name, strlen(name) + 1); ++count;
    }
    events[i].serial = ++serial;
}
int wos_log_seen(const char *name) { return wos_log_seen_since(name, 0) != 0; }
uint64_t wos_log_serial(void) { return serial; }
uint64_t wos_log_seen_since(const char *name, uint64_t since)
{
    size_t i;
    for (i = 0; i < count; ++i)
        if (!strcmp(events[i].name, name)) return events[i].serial > since ? events[i].serial : 0;
    return 0;
}
/* The dump pair as a log line: `EVT hero.level hero.level=5`. The key is both
 * the event name and half the payload, so a .dsc `dump` op and a plain log grep
 * see the same tokens. */
void log_emit(const char *key, const char *value, void *user)
{
    (void)user;
    if (key && value) wos_log_event(key, "%s=%s", key, value);
}
