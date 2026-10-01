/* Solo Channel chat pane and the offline slash/console command dispatcher.
 *
 * The original's chat edit is FUN_00434f95 (0x434f95). It first tries FUN_00434756
 * (0x434756), a dispatcher of the commands that are safe with no SRNet connection, then falls
 * through its own chain of the rest. Both chains are reproduced here in the original's order,
 * with the original's comparison style kept: _stricmp for the whole word, _strnicmp for the
 * word plus its trailing space (so "/tune 5" and "/tune 5x" both match "tune ").
 *
 * Offline the only channel is the Solo Channel (the channel list at 0x4e8214 also names
 * "Mplayer Channel" and "Modem Channel", neither of which exists without a connection). Owner:
 * chat.c. */
#ifndef WOS_CHAT_H
#define WOS_CHAT_H

#include "../engine/dump.h"
#include "../engine/ui.h"

#define CHAT_LOG_LINES   64
#define CHAT_LINE_MAX    256
#define CHAT_INPUT_MAX   256

/* The game version the /version dialog shows (dialog resource 0xCC, "Well of Souls Version 1.0"). */
#define CHAT_GAME_VERSION "Well of Souls Version 1.0"

void chat_init(void);

/* The original's chat edit. Returns 1 when the line was consumed locally (a recognized command
 * or a message echoed to the pane) and 0 when it would have gone to the network, which cannot
 * happen offline. FUN_00434f95's return value is left uninitialized by the original on some
 * matched commands; 1 is the useful reading and is what the callers test. */
int  chat_submit(const char *line);
/* Handle a pane Enter. Returns 1 when a line was submitted. */
int  chat_submit_input(void);

/* Pane. The splitter strip is the top 4 rows of the pane rect; clicking it opens and closes. */
void chat_layout(int x, int y, int w, int h);
int  chat_is_open(void);
/* Returns 1 when the pane consumed the event. */
int  chat_update(const Input *input);
void chat_render(Framebuffer *fb);

int         chat_log_count(void);
const char *chat_log_line(int index);
void        chat_say(const char *text);
void        chat_echo(const char *name, const char *text);  /* a scene or system line */

/* Debug-menu and window toggles, all of which live in the original's globals. */
int chat_overlay_terrain(void);   /* /terrain  -> DAT_004df8b4 */
int chat_overlay_monsters(void);  /* /monsters -> DAT_004df8b8 */
int chat_overlay_coord(void);     /* /coord    -> DAT_004e4828 */
int chat_share(void);             /* /share    -> DAT_004e6f64 */
int chat_eavesdrop(void);         /* /eavesdrop-> DAT_004e6f70 */
int chat_seance(void);            /* /seance   -> DAT_004e6f6c */
int chat_gossip(void);            /* /g        -> DAT_004e70a0 */
int chat_show_fps(void);          /* /fps      -> DAT_004e6f7c */
int chat_peek(void);              /* /peek     -> DAT_004e6f68 */
int chat_channel_open(void);      /* /ouvrir   -> DAT_004e6f60 */
int chat_tune(void);              /* /tune N   -> DAT_004e6f74 */
int chat_gimme_budget(void);      /* /gimme    -> spendable budget gate */

/* The /springy world (FUN_0041e1aa + FUN_0047392e + FUN_00473967), parsed from
 * <world>/springy.ini. 0 on success. */
int chat_springy_load(const char *ini_name);
/* The parsed /springy world. chat_springy_loaded() is 0 until a /springy has succeeded. */
int chat_springy_loaded(void);
int chat_springy_object_count(void);
int chat_springy_masses(int object);      /* numMasses of [Object<object>] */
/* mass `index` of `object` as kg, relative x, relative y, radius. 0 on success. */
int chat_springy_mass(int object, int index, double mass[4]);
/* The dice roller behind /dice and /pdice (FUN_00434574). Writes the roll into `out`. */
void chat_roll_dice(const char *spec, int is_public, char *out, size_t out_size);

void chat_dump(DumpEmit emit, void *user);

#endif
