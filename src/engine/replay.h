#ifndef WOS_REPLAY_H
#define WOS_REPLAY_H
#include <stddef.h>
#include <stdint.h>
#include "../platform/platform.h"
#define REPLAY_COMMAND_MAX 4096
#define REPLAY_EVENTS_MAX 40
typedef enum { RP_WAIT, RP_KEY, RP_DOWN, RP_UP, RP_HOLD, RP_TEXT, RP_CLICK, RP_MOVE, RP_EXPECT, RP_QUIT } ReplayOp;
typedef struct { ReplayOp op; int key,x,y,button; uint32_t frames; char *text; } ReplayCommand;
typedef struct {
    ReplayCommand commands[REPLAY_COMMAND_MAX];
    size_t count,pc;
    uint32_t remaining,expect_elapsed;
    PlatEvent pending;
    uint64_t observed;
    const char *failed_event;
} Replay;
/* Mutates/borrows text; returns 0 or -1 and the 1-based failing line. */
int replay_parse(Replay *replay, char *text, size_t *error_line);
int replay_key(const char *name);
typedef int (*ReplaySeen)(const char *event, uint64_t since, void *user);
/* One fixed frame. events must have REPLAY_EVENTS_MAX slots. Pure except caller callback.
 * Returns 0 active, 1 complete/quit, 2 expect timeout. serial is caller's latest event serial.
 * Check output count even on completion. Replay initializes observed to 0. */
int replay_step(Replay *replay, PlatEvent events[REPLAY_EVENTS_MAX], size_t *count,
                uint64_t serial, ReplaySeen seen, void *user);
#endif
