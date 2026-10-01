/* Environmental sound themes (FUN_00456D2F and its table) and the SRN boot mixer of
 * FUN_0042B4E0. Both run in offline solo play, and both consume crt_rand(), so the
 * port carries them exactly. Owner: Core. */
#ifndef WOS_SCHED_H
#define WOS_SCHED_H

#include <stddef.h>
#include <stdint.h>

/* FUN_00456B87's cap: the theme table holds at most 0x80 one-shots. */
#define ENV_ENTRIES_MAX 128
/* +THEMES ids are 0..255 (SoundThemesParser 0x48282E), 20 one-shots each. */
#define ENV_THEMES_MAX 256
#define ENV_THEME_SHOTS 20

/* Sound ids of FUN_00429A31: 0..65 index the built-in table at 0x4E62E0;
 * 1000..1255 are the world's own names, registered by FUN_00429BCC. */
#define ENV_SOUND_CUSTOM_BASE 1000
#define ENV_SOUND_CUSTOM_MAX 256

/* FUN_00429A31's play modes (FUN_0046890D): stop, restart once, loop. */
enum { ENV_PLAY_STOP = 0, ENV_PLAY_ONCE = 1, ENV_PLAY_LOOP = 2 };

/* FUN_00429BCC: the custom id of a sound name, registering it in the first free slot.
 * The name is lowercased and gets ".wav" unless it already contains it. -1 when full. */
int  env_sound_id(const char *name);
/* FUN_00429A31: play, restart or stop sound `id`. Returns -1 when nothing plays. */
int  env_sound_play(int id, int mode);

/* SoundThemesParser (0x48282E): rebuild the world's +THEMES table. Call after a world
 * loads; the custom sound registry is NOT reset here (the original resets it at boot). */
void env_world_loaded(void);
/* FUN_00456D2F: select sound theme `theme` (the THEME opcode, a map's arg5, the title's 2).
 * Gated by enableSFX, enableEnvironmentalSounds and enableSoundCard. */
void env_theme(int theme);
/* FUN_00456AA1: the per-world-step pass that fires due one-shots and re-arms them. */
void env_tick(void);
/* The theme last passed to env_theme(), _DAT_004F0E28. */
int  env_current_theme(void);
/* The active table, for dumps and tests: count, or -1 when no theme is installed. */
int  env_active_count(void);

/* FUN_0042B4E0: five crt_rand() plus clock_ms() per iteration, looping while the
 * fold is exactly zero. Five is the usual count, not a fixed one. */
void srn_mix(void);

#endif
