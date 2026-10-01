/* WoS A96 solo front end, public surface.
 *
 * The original's front end is FUN_0041b891 (0x41B891): one function sets up a
 * state, the state is the global DAT_004df8a4, and the per-tick FUN_0041bdb4
 * (0x41BDB4) drives it. See docs/re/boot_flow.md section 1 for the state table
 * and section 2 for the clickable-text hotspot system (array DAT_005339F8,
 * stride 0xBC, per-mille coordinates). Owner: front.c. */
#ifndef WOS_FRONT_H
#define WOS_FRONT_H

#include "../engine/dump.h"

/* Original state values of DAT_004df8a4, all.c:21078 (FUN_0041b891's switch).
 * The port keeps the same numbering so a dump reads like the decomp. */
enum {
    FRONT_TITLE     = 0, /* art\title.jpg,  auto-advances to MENU after 13000 ms  */
    FRONT_MENU      = 1, /* art\beg.jpg,    main menu, plays tos.rtf              */
    FRONT_WHERE     = 2, /* art\where.jpg,  "Where Do You Want To Play Today?"    */
    FRONT_CHOOSE    = 3, /* art\chapter.jpg,"Choose Your World" + the world list   */
    FRONT_STORY     = 4, /* the +STORY scroller                                 */
    FRONT_WELL      = 5, /* in-world / the Well: Pick a Soul + button bar        */
    FRONT_SCENE     = 6, /* scene entered by link                                */
    FRONT_JOIN      = 7, /* scene entered by join-scene                           */
    FRONT_DEATH     = 8, /* art\death.jpg,  click -> FRONT_WELL                   */
    FRONT_WE_WORLD  = 9, /* world editor, delegated to Editors                    */
    FRONT_WE_LINK   = 10,/* link editor, delegated to Editors                    */
    FRONT_WEB       = 11 /* IWebBrowser2 view; the port opens it externally      */
};

/* Current value of DAT_004df8a4. FRONT_WEB is reported while the external
 * browser is being launched, so a dump can tell the two apart. */
int front_state(void);
/* 1 while any of the 13 states is showing. */
int front_active(void);
/* SRNet's 0x46F modal is up. Its modal loop dispatches messages but never runs AppRun's
 * idle path, so FUN_0040A7C7 is entered only from the 100 ms timer meanwhile. */
int front_modal_up(void);

/* Transition to FRONT_TITLE from a cold boot. Returns 0 on success. */
int front_enter_title(void);

/* Re-apply the current state's setup, as FUN_0041b891 does on every entry:
 * free both hotspot layers, reload the state's art, re-register its hotspots
 * and stamp the state tick. Used after a world load or an editor hand-off. */
void front_reenter(void);

/* --- Replay/dialog bridge -------------------------------------------------
 * Core owns the .dsc `dialog` op and routes ids it does not own here:
 *   at <ms> dialog <id> <control>=<value>... ok|cancel
 * Register the handler that receives it. `kv` is `n` "control" strings in
 * "control=value" form, exactly as written in the script; the handler must not
 * retain the pointers. Returns the previous handler.
 *
 * Dialog 138 (0x8A) is the New Soul dialog (CListBox class list at dlg+0x2AC,
 * CComboBox gender at +0xAC, CEdit name at +0x32C, PK checkbox at +0x30C, OK at
 * +0x2EC; FUN_004603B4 / FUN_00460A7E). Dialog 149 (0x95) is the post-creation
 * follow-up (FUN_004448E0F). Front.c accepts both. */
typedef int (*FrontDialogFn)(int dialog_id, const char *const *kv, int n, int ok);
void front_dialog_register(FrontDialogFn fn);
/* What Core's `dialog` op calls for the ids front.c owns. Forwards to the
 * registered handler when there is one, otherwise to front.c's own. */
int  front_dialog_op(int dialog_id, const char *const *kv, int n, int ok);
/* --- the personal BIO panel ------------------------------------------------
 * FUN_00452107 (0x452107) is the bio editor's commit handler, reached from
 * FUN_00450E94, FUN_00450DBC and FUN_00452DF0. It is a custom CWnd, not a
 * dialog resource - the same shape as the Pick-a-Soul window - so there is no
 * resource ID and no control-ID table to copy; the original's controls are the
 * bio edit at CWnd+0x338 and the home/guild URL edit at +0x3F8, and the button
 * that opens it is labelled "Edits" (0x4F02C4) and sits on the "Where would you
 * like to play" screen, i.e. the world-select state.
 *
 * front_bio_op() is the semantic dialog op for it. Because the original has no
 * dialog resource the port uses its own pseudo-id and its own control names; a
 * .dsc that drives the original's BIO has to click the same pixels instead.
 * `text` is the body; ok commits through hero_bio_save() and cancel discards. */
#define FRONT_DIALOG_BIO 215   /* the port's own id; the original has no resource */
int  front_bio_op(const char *text, int ok);
/* 1 while the BIO panel is up. */
int  front_bio_active(void);

/* The front end's own state for the differential dump: the hotspot table, which
 * the original keeps at DAT_005339F8 (0xBC bytes per record) and hit-tests
 * clicks against directly, so the table IS the state. Emits
 * front.hotspot_count, front.hotspot.<i>.{state,clickable,msg,target,rect,label}
 * and front.client_{w,h}. Does not allocate. Core registers this as "front". */
void front_dump(DumpEmit emit, void *user);


/* --- Death bridge --------------------------------------------------------
 * Battle-2 calls this when the local hero dies (FUN_00494FCD, all.c:109303).
 * The original does not open a window here: it prints "%s has been killed" and,
 * when DAT_00502a48 is clear, the view goes to state 8 (art\death.jpg) whose
 * mouse-down is FUN_0041b891(5) (all.c:21411). `killer` may be NULL. */
void front_hero_death(const char *killer);

#endif
