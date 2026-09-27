/* Scene-local map rules (FLAGS; docs/re/script.md section 2.2). */
#ifndef WOS_SCENE_H
#define WOS_SCENE_H
#include <stdint.h>
uint32_t scene_flags(void);
/* Fixed-step timers continue across map/scene transitions, and reset on hero switch. */
void scene_tick(void);
void scene_reset_timers(void);
#endif
