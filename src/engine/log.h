#ifndef WOS_LOG_H
#define WOS_LOG_H
#include <stdint.h>
/* NULL path selects stdout. Returns 0 on success. */
int wos_log_open(const char *path);
void wos_log_close(void);
void wos_log_event(const char *name, const char *fmt, ...);
int wos_log_seen(const char *name);
uint64_t wos_log_serial(void);
/* Serial of the latest `name` event if newer than `serial`, else 0. */
uint64_t wos_log_seen_since(const char *name, uint64_t serial);
#endif
