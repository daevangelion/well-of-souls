/* The world HTML viewer used by the HTML opcode (0x29) and by item class 201.
 *
 * Original: FUN_0048A69E (VA 0x48a69e) is the whole entry point. It builds a URL and hands it
 * to the IE browser control hosted as a full-client-area child of the main window (child id
 * 201; FUN_0041B476, VA 0x41b476, MoveWindow's it to 0,0,cw,ch). The scene machine then parks in
 * state 10 (FUN_00490E7C, VA 0x490e7c case 10) until FUN_0043891C (VA 0x43891c) reports the
 * browser window no longer visible.
 *
 * The port replaces the IE control with an in-framebuffer renderer over the same file, so the
 * page is drawn with the 8x8 font and the game's image loader. See
 * docs/architecture_port.md "Deliberate deviations".
 * Owner: html.c. */
#ifndef WOS_HTML_H
#define WOS_HTML_H

#include "../engine/dump.h"
#include "../engine/ui.h"

/* FUN_0048A69E. `arg` is the opcode argument / items.txt arg15 verbatim:
 *   _strnicmp(arg, "http://", 7) == 0  -> the argument is the URL, used as given;
 *   otherwise                          -> "<data>\worlds\<world>\HTML\<arg>", and a missing
 *                                         file fails (returns 0, no viewer).
 * Returns 1 when the viewer opened, 0 otherwise. Emits html_open / html_missing. */
int html_open(const char *arg);

/* FUN_0043891C: 1 while the viewer is on screen. The scene VM polls this in state 10. */
int html_active(void);

/* Input and drawing. Call both every loop iteration while html_active(). */
void html_update(const Input *input);
void html_render(Framebuffer *fb);

/* Leave the viewer (resumes the suspended script). Idempotent. */
void html_close(void);
/* Release the parsed page, images and links. Does not close an open viewer implicitly beyond
 * html_close(); call html_close() first if the VM is parked. */
void html_free(void);

/* The URL the original would have handed the browser control, "" when nothing is loaded. */
const char *html_url(void);

void html_dump(DumpEmit emit, void *user);

#endif
