/* State-dump plumbing for the differential replay.
 *
 * Every module that owns a slice of the original's state exposes
 *   void <module>_dump(DumpEmit emit, void *user);
 * and emits `key=value` lines. The keys are flat and namespaced
 * (`hero.*`, `rng.*`, `clock.*`, `battle.*`, `scene.*`, `map.*`); the Oracle
 * owns the registry in docs/re/oracle.md and the port side in
 * src/game_main.c (`--dump FILE` and the `at <ms> dump <label>` script op).
 *
 * Modules MUST NOT allocate: emit into the caller's buffer. Owner: Core. */
#ifndef WOS_DUMP_H
#define WOS_DUMP_H

typedef void (*DumpEmit)(const char *key, const char *value, void *user);

/* Convenience for modules: fmt a numeric key. Returns through *out. */
void dump_emit_int(DumpEmit emit, const char *key, long long value, void *user);

#endif
