#ifndef WOS_GAME_MAIN_H
#define WOS_GAME_MAIN_H
int game_main(int argc, char **argv);
int game_boot(void); /* supplied by the game flow; 0 success */
const char *game_data_path(void);
const char *game_save_path(void); /* --save DIR, or <data>/Save; game owns directory creation */
void game_request_quit(void);
/* DAT_004DEA1C: the pet pen dialog (#5) is up. CSoulsView::OnCreate (0x41AF0A) sets it at
 * boot through FUN_00412716; ToggleGameDialog sets it on opening #5 and FUN_00412797
 * clears it on closing. With option 10 on, the world step runs the front end on every
 * other 25 ms step while it is set (NetGraphTick, 0x428B1D). */
extern int g_pet_pen_up;
#endif
