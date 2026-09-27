#include <SDL.h>
#include "game_main.h"

/* SDL's Windows startup library forwards WinMain to this SDL_main. */
int main(int argc, char **argv)
{
    return game_main(argc, argv);
}
