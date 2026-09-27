/* Platform interface: the ONLY boundary between portable game code and the host OS.
 * Game code (src/ outside src/platform/) must include nothing OS- or SDL-specific;
 * everything host-dependent goes through these calls. One implementation per backend
 * lives under src/platform/<backend>/ (currently sdl2). */
#ifndef WOS_PLATFORM_H
#define WOS_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PLAT_SCREEN_W 640
#define PLAT_SCREEN_H 480

typedef enum {
    PLAT_EV_NONE = 0,
    PLAT_EV_QUIT,
    PLAT_EV_KEY_DOWN,   /* key = PlatKey or printable ASCII */
    PLAT_EV_KEY_UP,
    PLAT_EV_TEXT,       /* text[] = UTF-8 typed text */
    PLAT_EV_MOUSE_MOVE, /* x,y in framebuffer coordinates */
    PLAT_EV_MOUSE_DOWN, /* button = 1 left, 2 middle, 3 right */
    PLAT_EV_MOUSE_UP,
    PLAT_EV_MOUSE_WHEEL /* y = +1 up / -1 down */
} PlatEventType;

typedef enum {
    PLAT_KEY_BACKSPACE = 8,
    PLAT_KEY_TAB = 9,
    PLAT_KEY_RETURN = 13,
    PLAT_KEY_ESCAPE = 27,
    PLAT_KEY_SPACE = 32,
    /* printable ASCII 33..126 map to themselves (letters lower-case) */
    PLAT_KEY_DELETE = 127,
    PLAT_KEY_UP = 256,
    PLAT_KEY_DOWN,
    PLAT_KEY_LEFT,
    PLAT_KEY_RIGHT,
    PLAT_KEY_HOME,
    PLAT_KEY_END,
    PLAT_KEY_PAGEUP,
    PLAT_KEY_PAGEDOWN,
    PLAT_KEY_F1,
    PLAT_KEY_F12 = PLAT_KEY_F1 + 11,
    PLAT_KEY_SHIFT,
    PLAT_KEY_CTRL,
    PLAT_KEY_ALT
} PlatKey;

typedef struct {
    PlatEventType type;
    int key;
    int x, y;
    int button;
    char text[32];
} PlatEvent;

/* Lifecycle. flags: PLAT_INIT_HEADLESS hides the window (still works with SDL dummy driver). */
#define PLAT_INIT_HEADLESS 1u
int  plat_init(const char *title, int w, int h, unsigned flags); /* 0 on success */
void plat_shutdown(void);

/* Input: returns 1 and fills *ev while events are pending, 0 when the queue is empty. */
int plat_poll_event(PlatEvent *ev);
/* Text entry: enable while a text field has focus (shows the on-screen keyboard on touch devices). */
void plat_text_input(int enable);

/* Video: present a w*h framebuffer of 0x00RRGGBB pixels, letterboxed with aspect preserved. */
void plat_present(const uint32_t *pixels, int w, int h);

/* Time. */
uint32_t plat_ticks_ms(void);
void     plat_sleep_ms(uint32_t ms);

/* Files. Game data paths use '/' separators and are matched case-insensitively per
 * path component (the original shipped on case-insensitive Windows filesystems and
 * mixes case freely, e.g. "Monsters" vs "monsters"). */
FILE *plat_fopen(const char *path, const char *mode);
int   plat_mkdir(const char *path);                 /* 0 on success or already exists */
/* List directory entries (files and dirs, excluding . and ..). Calls cb for each name;
 * returns number of entries or -1 if the directory cannot be opened. */
int   plat_list_dir(const char *path, void (*cb)(const char *name, int is_dir, void *user), void *user);

/* Audio. Buffers are complete RIFF/WAVE files in memory; the platform decodes them.
 * Music is a path to a MIDI file; playback is best effort (silently ignored if unsupported). */
void plat_sound_play(const void *wav, size_t len);
void plat_music_play(const char *midi_path, int loop);
void plat_music_stop(void);
int plat_music_playing(void);

#endif
