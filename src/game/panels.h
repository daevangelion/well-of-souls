#ifndef WOS_PANELS_H
#define WOS_PANELS_H
#include "../engine/ui.h"
typedef enum { PANEL_NONE, PANEL_ITEMS, PANEL_SPELLS, PANEL_EQUIP, PANEL_STATS, PANEL_TRAIN, PANEL_SHOP } PanelKind;
void panel_open(PanelKind kind);
/* Consumes the temporary script arguments before returning. */
void panel_open_shop(const char *const *args, int argc, int offer2);
int panel_active(void);
void panel_update(const Input *input);
void panel_render(Framebuffer *fb);
void panel_close(void);
#endif
