#include <SDL.h>
#include "game_main.h"

#ifdef __ANDROID__
int wos_android_paths(char *data, char *save, size_t capacity);
#endif

/* SDL's Windows and Android startup libraries forward to this SDL_main. */
int main(int argc, char **argv)
{
#ifdef __ANDROID__
    char data[4096], save[4096];
    char *android_argv[] = { "wos", "--data", data, "--save", save, NULL };
    (void)argc;
    (void)argv;
    if (wos_android_paths(data, save, sizeof(data))) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Well of Souls",
                                 "Unable to install game data. Check available storage and relaunch.", NULL);
        return 1;
    }
    return game_main(5, android_argv);
#else
    return game_main(argc, argv);
#endif
}
