#ifndef WOS_GAME_MAIN_H
#define WOS_GAME_MAIN_H
int game_main(int argc, char **argv);
int game_boot(void); /* supplied by the game flow; 0 success */
const char *game_data_path(void);
void game_request_quit(void);
#endif
