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

/* Preference option ids, from the 33-entry table at DAT_004F2A58 that FUN_00466EF3 reads into
 * DAT_006840D0 and FUN_00467312 (VA 0x467312) indexes. */
#define HTML_OPTION_SHOW_IN_SCENES 22 /* "Show HTML pages in scenes, when scripted." */
#define HTML_OPTION_STOP_ON_RETURN 23 /* "Stop all web page stuff on return to game."   */

/* FUN_0048A69E, gated by preference option 22. `arg` is the opcode argument / items.txt arg15
 * verbatim:
 *   _strnicmp(arg, "http://", 7) == 0  -> the argument is the URL, used as given;
 *   otherwise                          -> "<data>\worlds\<world>\HTML\<arg>", and a missing
 *                                         file fails (returns 0, no viewer).
 * Option 22 off also returns 0 (FUN_00467312(0x16)), and the script simply advances.
 * Emits html_open / html_missing / html_disabled. */
int html_open(const char *arg);

/* FUN_0043891C: 1 while the viewer is on screen. The scene VM polls this in state 10. */
int html_active(void);

/* Input and drawing. Call both every loop iteration while html_active(). */
void html_update(const Input *input);
void html_render(Framebuffer *fb);

/* Leave the viewer (resumes the suspended script). Idempotent. With preference option 23
 * ("Stop all web page stuff on return to game.") the page itself is dropped too, as
 * FUN_0045BC8D -> FUN_0045BA34 does; with it off the page stays loaded, as the browser
 * control keeps it. */
void html_close(void);
/* Always releases the parsed page, images and links. */
void html_free(void);

/* The URL the original would have handed the browser control, "" when nothing is loaded. */
const char *html_url(void);

void html_dump(DumpEmit emit, void *user);

#endif
