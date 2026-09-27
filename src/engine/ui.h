#ifndef WOS_UI_H
#define WOS_UI_H
#include <stddef.h>
#include "fb.h"
#include "../platform/platform.h"
#define INPUT_KEYS 512
#define INPUT_TEXT_MAX 1024
typedef struct Input {
    unsigned char down[INPUT_KEYS], pressed[INPUT_KEYS], released[INPUT_KEYS];
    int mouse_x, mouse_y;
    unsigned mouse_down, mouse_pressed, mouse_released; /* bit (1u << button) */
    int wheel, quit;
    char text[INPUT_TEXT_MAX];
} Input;
void input_begin(Input *input); /* clears edges, retains held states */
void input_event(Input *input, const PlatEvent *event);
typedef struct { const Input *input; unsigned active, focus; } Ui;
void ui_begin(Ui *ui, const Input *input); /* initialize Ui to {0} once */
int ui_button(Ui *ui, Framebuffer *fb, unsigned id, Rect rect, const char *label);
/* Returns 1 on Return while focused; editing is immediate. IDs must be nonzero. */
int ui_text_input(Ui *ui, Framebuffer *fb, unsigned id, Rect rect, char *text, size_t capacity);
/* Returns 1 when selection changes. first is the first displayed item. */
int ui_list(Ui *ui, Framebuffer *fb, unsigned id, Rect rect, const char *const *items, int count, int first, int *selection);
void ui_panel(Framebuffer *fb, Rect rect, const char *title, const char *message);
#endif
