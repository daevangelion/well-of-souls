#ifndef WOS_GAME_MAIN_H
#define WOS_GAME_MAIN_H
int game_main(int argc, char **argv);
int game_boot(void); /* supplied by the game flow; 0 success */
const char *game_data_path(void);
const char *game_save_path(void); /* --save DIR, or <data>/Save; game owns directory creation */
void game_request_quit(void);
#endif
