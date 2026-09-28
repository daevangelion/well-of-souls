/* World missions: the missions.ini reader, the per-hero mission log and the mission picker.
 *
 * Original: every mission is a `[<job>]` section of worlds/<W>/missions.ini, read with
 * GetPrivateProfileStringA against "%s\worlds\%s\missions.ini" (FUN_004586D9, VA 0x4586d9; path
 * format string at 0x4f1268). The player's progress is NOT in the hero record: it lives in the
 * hero's own INI, "<save>\<world>\savedHeroes\<name>.mis", section "<job>", key "Status"
 * (FUN_00458BB6 read / FUN_00458C63 write, VA 0x458bb6 / 0x458c63; the file name is built by
 * FUN_00460962 with the extension "mis" at 0x4f1264). Status values and their labels are in
 * FUN_00458D1E (VA 0x458d1e): 0 Available, 1 "In Progress", 2 "Complete", 3 "Done".
 *
 * Owner: missions.c. */
#ifndef WOS_MISSIONS_H
#define WOS_MISSIONS_H

#include "../engine/dump.h"
#include "../engine/ui.h"

/* Status of one mission for the current hero (FUN_00458D1E labels). */
enum {
    MISSION_AVAILABLE = 0,
    MISSION_PROGRESS  = 1,  /* "In Progress"  */
    MISSION_COMPLETE  = 2,  /* "Complete": the trophy requirement is met */
    MISSION_DONE      = 3   /* "Done": the reward has been paid */
};

/* The scene IF-condition evaluator (FUN_004859A2, VA 0x4859a2). missions.ini's `Qualify` is the
 * same condition language, and JQ runs it. Supplied by the script VM. */
typedef int (*MissionCondFn)(const char *condition, void *user);
void missions_set_condition_evaluator(MissionCondFn fn, void *user);

/* The GIVE/TAKE object handler (FUN_00484E72, VA 0x484e72): a spec is "<letter><id>[.<count>]"
 * and a negative count takes. Every Reward/Accept/Abandon Give/Take in missions.ini uses it.
 * Supplied by the script VM. */
typedef int (*MissionGiveFn)(const char *spec, int count, void *user);
void missions_set_give_handler(MissionGiveFn fn, void *user);

/* Read worlds/<W>/missions.ini. Called lazily on first use; call it once after world_load() to
 * fail early. 0 on success (a world with no missions.ini is not an error). */
int  missions_load(void);
void missions_free(void);

/* 1 when [job] exists in missions.ini (FUN_0045862D lists the sections, VA 0x45862d). */
int missions_defined(int job);
/* A key of that section (Name, Desc, Trophies, RewardGold, Qualify, ...), "" when absent. */
const char *missions_field(int job, const char *key);
int missions_count(void);
int missions_job_at(int index);

/* The hero's mission log. */
int  mission_status(int job);                          /* MISSION_AVAILABLE when unknown */
void mission_set_status(int job, int status);
void mission_clear(int job);                           /* drop the whole [job] section */
void mission_reload(void);                             /* after a hero switch */

/* The IF conditions of FUN_004851F1 case 0x4A (VA 0x4851f1, all.c:63793). */
int mission_completed(int job);   /* JF: status == 3                    (FUN_00458616) */
int mission_accepted(int job);    /* JA: status == 1 or 2               (FUN_0045AB4B) */
int mission_qualified(int job);   /* JQ: not completed and Qualify holds (FUN_0045A9C7) */
/* Trophies="5x1,10x16" all held (FUN_004587C3 + FUN_00458923, VA 0x4587c3 / 0x458923). */
int mission_trophies_met(int job);

/* MISSION / MISSIONS (one opcode, 0x4A). The VM has already turned every argument into "%04X";
 * `jobs` are the parsed job numbers. This is FUN_00478D53 (VA 0x478d53): it records the offered
 * list and arms scene button-bar slot 6 with buttonMission.bmp (message 0x53C). count == 0 is
 * the Book of Missions (FUN_004213E8, dialog resource 0xED). */
void missions_offer(const int *jobs, int count);
int  missions_offered(void);
const int *missions_offer_list(int *count);
void missions_clear_offer(void);

/* What the Mission button does. Also opens the Book of Missions when nothing is offered. */
void missions_open_picker(void);
int  missions_panel_active(void);
void missions_panel_update(const Input *input);
void missions_panel_render(Framebuffer *fb);
void missions_panel_close(void);

void missions_dump(DumpEmit emit, void *user);

#endif
