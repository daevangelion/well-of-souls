#ifndef WOS_SCREEN_H
#define WOS_SCREEN_H
#include "ui.h"
typedef struct Screen {
    const char *name;
    void (*enter)(void);
    void (*update)(const Input *);
    void (*render)(Framebuffer *);
    void (*leave)(void);
} Screen;
/* Screen descriptors must outlive their registration. Transitions are immediate. */
void screen_set(const Screen *screen);
const Screen *screen_current(void);
#endif
