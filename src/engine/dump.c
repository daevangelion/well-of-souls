#include "dump.h"
#include <stdio.h>

void dump_emit_int(DumpEmit emit, const char *key, long long value, void *user)
{
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%lld", value);
    if (n < 0) return;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    emit(key, buf, user);
}
