#include "platform/platform.h"
#include <SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

void wos_audio_init(void);
void wos_audio_shutdown(void);

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int frame_w, frame_h;

int plat_init(const char *title, int w, int h, unsigned flags)
{
    Uint32 window_flags = SDL_WINDOW_RESIZABLE |
        ((flags & PLAT_INIT_HEADLESS) ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    if (w <= 0 || h <= 0 || w > INT_MAX / 4 || window) return -1;
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) < 0) return -1;
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w, h, window_flags);
    if (!window) goto fail;
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) goto fail;
    if (SDL_RenderSetLogicalSize(renderer, w, h) < 0) goto fail;
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888,
                                SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!texture) goto fail;
    frame_w = w;
    frame_h = h;
    SDL_StopTextInput();
    wos_audio_init();
    return 0;
fail:
    fprintf(stderr, "Platform initialization failed: %s\n", SDL_GetError());
    plat_shutdown();
    return -1;
}

void plat_text_input(int enable)
{
    if (enable) {
        if (!SDL_IsTextInputActive()) SDL_StartTextInput();
    } else if (SDL_IsTextInputActive()) {
        SDL_StopTextInput();
    }
}

void plat_shutdown(void)
{
    wos_audio_shutdown();
    SDL_StopTextInput();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    texture = NULL;
    renderer = NULL;
    window = NULL;
    frame_w = frame_h = 0;
    SDL_Quit();
}

static int translate_key(SDL_Keycode key)
{
    if (key >= 8 && key <= 127) return (int)key;
    if (key >= SDLK_F1 && key <= SDLK_F12) return PLAT_KEY_F1 + (int)(key - SDLK_F1);
    switch (key) {
    case SDLK_UP: return PLAT_KEY_UP;
    case SDLK_DOWN: return PLAT_KEY_DOWN;
    case SDLK_LEFT: return PLAT_KEY_LEFT;
    case SDLK_RIGHT: return PLAT_KEY_RIGHT;
    case SDLK_HOME: return PLAT_KEY_HOME;
    case SDLK_END: return PLAT_KEY_END;
    case SDLK_PAGEUP: return PLAT_KEY_PAGEUP;
    case SDLK_PAGEDOWN: return PLAT_KEY_PAGEDOWN;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return PLAT_KEY_SHIFT;
    case SDLK_LCTRL: case SDLK_RCTRL: return PLAT_KEY_CTRL;
    case SDLK_LALT: case SDLK_RALT: return PLAT_KEY_ALT;
    case SDLK_KP_ENTER: return PLAT_KEY_RETURN;
    case SDLK_AC_BACK: return PLAT_KEY_ESCAPE;
    default: return 0;
    }
}

static void mouse_position(PlatEvent *ev, int x, int y)
{
    /* SDL_RenderSetLogicalSize already maps window/touch events into the letterboxed viewport. */
    ev->x = x;
    ev->y = y;
}

int plat_poll_event(PlatEvent *ev)
{
    SDL_Event event;
    if (!ev || !window) return 0;
    while (SDL_PollEvent(&event)) {
        memset(ev, 0, sizeof(*ev));
        switch (event.type) {
        case SDL_QUIT: ev->type = PLAT_EV_QUIT; return 1;
        case SDL_KEYDOWN: case SDL_KEYUP:
            ev->key = translate_key(event.key.keysym.sym);
            if (!ev->key) continue;
            ev->type = event.type == SDL_KEYDOWN ? PLAT_EV_KEY_DOWN : PLAT_EV_KEY_UP;
            return 1;
        case SDL_TEXTINPUT:
            ev->type = PLAT_EV_TEXT;
            memcpy(ev->text, event.text.text, sizeof(ev->text));
            ev->text[sizeof(ev->text) - 1] = '\0';
            return 1;
        case SDL_MOUSEMOTION:
            ev->type = PLAT_EV_MOUSE_MOVE;
            mouse_position(ev, event.motion.x, event.motion.y);
            return 1;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
            ev->type = event.type == SDL_MOUSEBUTTONDOWN ? PLAT_EV_MOUSE_DOWN : PLAT_EV_MOUSE_UP;
            ev->button = event.button.button;
            mouse_position(ev, event.button.x, event.button.y);
            return 1;
        case SDL_MOUSEWHEEL:
            ev->type = PLAT_EV_MOUSE_WHEEL;
            ev->y = (event.wheel.y > 0) - (event.wheel.y < 0);
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ev->y = -ev->y;
            return 1;
        default: break;
        }
    }
    return 0;
}

void plat_present(const uint32_t *pixels, int w, int h)
{
    if (!renderer || !pixels || w <= 0 || h <= 0 || w > INT_MAX / 4) return;
    if (w != frame_w || h != frame_h) {
        SDL_Texture *replacement = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888,
                                                     SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!replacement) return;
        if (SDL_RenderSetLogicalSize(renderer, w, h) < 0) {
            SDL_DestroyTexture(replacement);
            return;
        }
        SDL_DestroyTexture(texture);
        texture = replacement;
        frame_w = w;
        frame_h = h;
    }
    if (SDL_UpdateTexture(texture, NULL, pixels, w * 4) < 0) return;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

uint32_t plat_ticks_ms(void) { return SDL_GetTicks(); }
static uint32_t wos_time_pin;
uint32_t plat_time_s(void) { return wos_time_pin ? wos_time_pin : (uint32_t)time(NULL); }
uint32_t plat_time_set_s(uint32_t epoch_s)
{
    uint32_t was = wos_time_pin;
    wos_time_pin = epoch_s;
    return was;
}
void plat_sleep_ms(uint32_t ms) { SDL_Delay(ms); }

/* --- External targets and cursors -----------------------------------------
 * The original hands URLs and the help file to ShellExecute. SDL has no shell
 * hook, so the backend asks the OS the portable way: xdg-open / open / cmd
 * start, chosen by the build platform. When none applies (headless, Android,
 * a container with no helper) the call fails cleanly and the game logs it. */
static PlatCursor cursor_shape = PLAT_CURSOR_DEFAULT;

int plat_open_external_supported(void)
{
#if defined(__ANDROID__) || defined(_WIN32)
    return 1;
#else
    const char *d = getenv("DISPLAY");
    const char *w = getenv("WAYLAND_DISPLAY");
    return (d && *d) || (w && *w);
#endif
}

int plat_open_external(const char *target)
{
    if (!target || !*target) return -1;
#if defined(__ANDROID__)
    (void)target;
    return -1;
#elif defined(_WIN32)
    int ok = (system(NULL) != 0);
    char cmd[1200];
    int n = snprintf(cmd, sizeof(cmd), "start \"\" \"%s\"", target);
    if (n < 0 || (size_t)n >= sizeof(cmd)) return -1;
    if (!ok) return -1;
    return system(cmd) == 0 ? 0 : -1;
#else
    static const char *const helpers[] = { "xdg-open", "gio", "gnome-open", "kde-open", NULL };
    int i;
    for (i = 0; helpers[i]; ++i) {
        char cmd[1200];
        int n = snprintf(cmd, sizeof(cmd), "%s \"%s\" >/dev/null 2>&1 &", helpers[i], target);
        if (n < 0 || (size_t)n >= sizeof(cmd)) return -1;
        if (system(cmd) == 0) return 0;
    }
    return -1;
#endif
}

int plat_cursor_set(PlatCursor shape)
{
    if (!window) return -1;
    switch (shape) {
    case PLAT_CURSOR_HAND: SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND)); break;
    case PLAT_CURSOR_BUSY: SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAIT)); break;
    case PLAT_CURSOR_TEXT: SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM)); break;
    default: break;
    }
    cursor_shape = shape;
    return 0;
}

void plat_cursor_restore(void)
{
    if (window && cursor_shape != PLAT_CURSOR_DEFAULT) {
        SDL_SetCursor(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));
        cursor_shape = PLAT_CURSOR_DEFAULT;
    }
}
