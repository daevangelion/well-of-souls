#include "screen.h"
static const Screen *current;
void screen_set(const Screen *screen)
{
    const Screen *old=current;
    if(old==screen) return;
    current=0;
    if(old && old->leave) old->leave();
    current=screen;
    if(current && current->enter) current->enter();
}
const Screen *screen_current(void) { return current; }
