/* In-framebuffer HTML viewer for the world's HTML/ folder.
 *
 * FUN_0048A69E (VA 0x48a69e) is the only entry point the original has. It builds a URL and hands
 * it to the IE browser control hosted as a full-client-area child of the main window (child id
 * 201; FUN_0041B476, VA 0x41b476, MoveWindow's it to 0,0,cw,ch). The scene machine then parks in
 * state 10 (FUN_00490E7C, VA 0x490e7c case 10) until FUN_0043891C (VA 0x43891c) reports the
 * browser window no longer visible.
 *
 * The port replaces the IE control with a layout over the same file, drawn with the 8x8 font and
 * the game's image loader; see docs/architecture_port.md "Deliberate deviations".
 * Owner: html.c. */
#include "html.h"
#include "options.h"
#include "world.h"
#include "../engine/font.h"
#include "../engine/log.h"
#include "../engine/text.h"
#include "../game_main.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HTML_MAX_LINES  600
#define HTML_MAX_LINKS  64
#define HTML_MAX_IMAGES 32
#define HTML_URL_MAX    1024
#define HTML_PATH_MAX   1024

#define HL_TEXT  0
#define HL_IMAGE 1

typedef struct {
    Image image;
    char path[HTML_PATH_MAX];
    int w, h;
} HtmlImage;

typedef struct {
    int kind, style, link, image, indent;
    char text[256];
} HtmlLine;

#define HS_CENTER 0x08
#define HS_RULE   0x10

#define PAGE_MARGIN  8
#define BAR_HEIGHT   24
#define BODY_COLOR   0x00000000u  /* black text */
#define PAGE_COLOR   0x00ffffffu  /* white page */
#define HEAD_COLOR   0x00204080u
#define LINK_COLOR   0x000000c0u
#define BAR_COLOR    0x00c0c0c0u
#define BAR_TEXT     0x00202020u
#define CLOSE_COLOR  0x00a03020u

static struct {
    int open;
    char url[HTML_URL_MAX];
    char page[HTML_PATH_MAX];  /* platform path of the loaded file, "" for a remote URL */
    char dir[HTML_PATH_MAX];   /* directory of the loaded file; relative links resolve here */
    char *text;
    HtmlLine lines[HTML_MAX_LINES];
    int line_count;
    char links[HTML_MAX_LINKS][256];
    int link_count;
    HtmlImage images[HTML_MAX_IMAGES];
    int image_count;
    int content_height, scroll;
} html;

static const Rect close_rect = {PLAT_SCREEN_W - 48, 4, 44, 16};

/* --- page geometry ---------------------------------------------------------- */

static int line_height(const HtmlLine *line)
{
    if (line->kind == HL_IMAGE)
        return (line->image >= 0 && line->image < html.image_count && html.images[line->image].h > 0)
                   ? html.images[line->image].h + 4 : 12;
    return (line->style & 0x07) ? FONT_H * 2 + 8 : FONT_H + 3;
}

static int body_width(int style, int indent)
{
    int cw = (style & 0x07) ? FONT_W * 2 : FONT_W;
    int n = (PLAT_SCREEN_W - 2 * PAGE_MARGIN - indent * FONT_W) / cw;
    return n > 8 ? n : 8;
}

/* --- parser ----------------------------------------------------------------- */

static HtmlLine *new_line(int kind)
{
    HtmlLine *line;
    if (html.line_count >= HTML_MAX_LINES) return 0;
    line = &html.lines[html.line_count++];
    memset(line, 0, sizeof *line);
    line->kind = kind;
    line->link = -1;
    return line;
}

/* Append one whitespace-delimited word, wrapping at the page width. */
static void emit_text(const char *word, int style, int link, int indent)
{
    HtmlLine *line;
    int max_chars = body_width(style, indent);
    int used, wlen;
    if (!*word) return;
    wlen = (int)strlen(word);
    line = &html.lines[html.line_count - 1];
    if (line->kind != HL_TEXT) {
        line = new_line(HL_TEXT);
        if (!line) return;
    }
    used = (int)strlen(line->text);
    /* A line carries at most one anchor, so the whole line is its hit box. Starting a new
     * line whenever the anchor changes keeps every link followable. */
    if (used && (used + 1 + wlen > max_chars || line->link != link)) {
        line = new_line(HL_TEXT);
        if (!line) return;
        used = 0;
    }
    if (used) line->text[used++] = ' ';
    memcpy(line->text + used, word, (size_t)wlen + 1);
    if (!line->style) line->style = style;
    line->link = link;
    if (line->indent < indent) line->indent = indent;
}

static void end_paragraph(void) { (void)new_line(HL_TEXT); }

static void add_rule(void)
{
    HtmlLine *line = new_line(HL_TEXT);
    if (line) line->style = HS_RULE;
}

static int add_link(const char *href)
{
    if (html.link_count >= HTML_MAX_LINKS || !*href) return -1;
    snprintf(html.links[html.link_count], sizeof html.links[0], "%s", href);
    return html.link_count++;
}

/* "<dir>/<name>" with an explicit bound; the tag attributes are untrusted. */
static int join_path(char *out, size_t cap, const char *dir, const char *name)
{
    size_t dn = strlen(dir), nn = strlen(name);
    if (dn + nn + 2 > cap) return -1;
    memcpy(out, dir, dn);
    if (name[0] == '/') { memcpy(out + dn, name, nn + 1); return 0; }
    out[dn] = '/';
    memcpy(out + dn + 1, name, nn + 1);
    return 0;
}

static void add_image(const char *src, int w, int h)
{
    char path[HTML_PATH_MAX];
    Image *image;
    int index;
    HtmlLine *line;
    if (html.image_count >= HTML_MAX_IMAGES || !*src) return;
    if (html.page[0]) { if (join_path(path, sizeof path, html.dir, src)) return; }
    else snprintf(path, sizeof path, "%s", src);
    index = html.image_count;
    line = new_line(HL_IMAGE);
    if (!line) return;
    line->image = index;
    snprintf(html.images[index].path, sizeof html.images[0].path, "%s", path);
    image = &html.images[index].image;
    if (image_load(image, path)) {
        wos_log_event("html_image_missing", "path=%s", path);
    } else {
        if (w <= 0) w = image->w;
        if (h <= 0) h = image->h;
    }
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    html.images[index].w = w;
    html.images[index].h = h;
    ++html.image_count;
    (void)new_line(HL_TEXT);
}

static int entity(const char **p)
{
    const char *s = *p;
    int n = 0, digits = 0;
    if (s[0] != '&') return 0;
    ++s;
    if (!strncmp(s, "lt", 2))   { *p = s + 2; return '<'; }
    if (!strncmp(s, "gt", 2))   { *p = s + 2; return '>'; }
    if (!strncmp(s, "amp", 3))  { *p = s + 3; return '&'; }
    if (!strncmp(s, "quot", 4)) { *p = s + 4; return '"'; }
    if (!strncmp(s, "apos", 4)) { *p = s + 4; return '\''; }
    if (!strncmp(s, "nbsp", 4)) { *p = s + 4; return ' '; }
    if (s[0] == '#') {
        ++s;
        if (*s == 'x' || *s == 'X') {
            ++s;
            while (isxdigit((unsigned char)*s)) {
                n = n * 16 + (isdigit((unsigned char)*s) ? *s - '0' : tolower((unsigned char)*s) - 'a' + 10);
                ++s; digits = 1;
            }
        } else {
            while (isdigit((unsigned char)*s)) { n = n * 10 + (*s - '0'); ++s; digits = 1; }
        }
        if (digits) { *p = s; return n & 0xff; }
    }
    return 0;
}

static int is_break(int c) { return c <= ' ' || c == ','; }

/* Scan one tag: returns the offset just past '>' (or the end of the text). */
static size_t tag_end(const char *p)
{
    while (*p && *p != '>') ++p;
    return (size_t)(*p ? p + 1 - html.text : p - html.text);
}

static void parse(void)
{
    char word[256];
    size_t wl = 0, pos;
    int style = 0, link = -1, indent = 0;
    html.line_count = 0;
    (void)new_line(HL_TEXT);
    for (pos = 0; html.text[pos];) {
        char c = html.text[pos];
        if (c == '<') {
            const char *body = html.text + pos + 1, *end;
            char name[32], href[256], src[256];
            size_t n = 0, i;
            int w = 0, h = 0, closing = 0;
            href[0] = src[0] = 0;
            if (*body == '/') { closing = 1; ++body; }
            while (isalpha((unsigned char)body[n]) && n + 1 < sizeof name) {
                name[n] = (char)tolower((unsigned char)body[n]); ++n;
            }
            name[n] = 0;
            pos = tag_end(html.text + pos);
            end = html.text + pos;
            /* attributes: name, optional '=', optional quoted value */
            i = (size_t)(body + n - html.text);
            while (i + 1 < (size_t)(end - html.text)) {
                char key[24], quote = 0;
                const char *v;
                size_t k = 0, l;
                while (i < (size_t)(end - html.text) && !isalpha((unsigned char)html.text[i])) ++i;
                while (i < (size_t)(end - html.text) && isalpha((unsigned char)html.text[i]) &&
                       k + 1 < sizeof key) {
                    key[k++] = (char)tolower((unsigned char)html.text[i]); ++i;
                }
                key[k] = 0;
                if (!k) break;
                while (i < (size_t)(end - html.text) && (html.text[i] == ' ' || html.text[i] == '=')) ++i;
                if (i < (size_t)(end - html.text) && (html.text[i] == '"' || html.text[i] == '\'')) {
                    quote = html.text[i]; ++i;
                }
                v = html.text + i;
                while (i < (size_t)(end - html.text) &&
                       (quote ? html.text[i] != quote : !is_break(html.text[i]) && html.text[i] != '=')) ++i;
                l = (size_t)(html.text + i - v);
                if (!strcmp(key, "href") && l < sizeof href) { memcpy(href, v, l); href[l] = 0; }
                else if (!strcmp(key, "src") && l < sizeof src) { memcpy(src, v, l); src[l] = 0; }
                else if (!strcmp(key, "width")) w = atoi(v);
                else if (!strcmp(key, "height")) h = atoi(v);
                if (quote) ++i;
            }
            if (wl) { word[wl] = 0; emit_text(word, style, link, indent); wl = 0; }
            if (!strcmp(name, "br")) end_paragraph();
            else if (!strcmp(name, "p") || !strcmp(name, "div") || !strcmp(name, "tr") ||
                     !strcmp(name, "li") || !strcmp(name, "blockquote")) end_paragraph();
            else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
                end_paragraph();
                style = name[1] - '0';
                indent = 0;
            } else if (!strcmp(name, "hr")) add_rule();
            else if (!strcmp(name, "center")) { if (closing) style &= ~HS_CENTER; else style |= HS_CENTER; }
            else if (!strcmp(name, "a")) {
                if (closing) link = -1;
                else if (href[0]) link = add_link(href);
            } else if (!strcmp(name, "img") && !closing) add_image(src, w, h);
            continue;
        }
        if (c == '&') {
            const char *q = html.text + pos;
            int decoded = entity(&q);
            if (decoded) {
                if (wl + 1 < sizeof word) word[wl++] = (char)decoded;
                pos = (size_t)(q - html.text);
                continue;
            }
        }
        if (is_break((unsigned char)c)) {
            if (wl) { word[wl] = 0; emit_text(word, style, link, indent); wl = 0; }
            if (c == '\t') ++indent;
            ++pos;
            continue;
        }
        if (wl + 1 < sizeof word) word[wl++] = c;
        ++pos;
    }
    if (wl) { word[wl] = 0; emit_text(word, style, link, indent); }
    html.content_height = 0;
    {
        int i;
        for (i = 0; i < html.line_count; ++i) {
            HtmlLine *line = &html.lines[i];
            if (line->kind == HL_IMAGE) line->text[0] = 0;
            html.content_height += line_height(line);
        }
    }
}

/* --- opening, following and closing ----------------------------------------- */

void html_free(void)
{
    int i;
    for (i = 0; i < html.image_count; ++i) image_free(&html.images[i].image);
    free(html.text);
    memset(&html, 0, sizeof html);
}

/* FUN_0045BC8D (VA 0x45bc8d) is the "we are back in the game" hook: option 23, "Stop all web page
 * stuff on return to game.", decides whether the loaded page is torn down (FUN_0045BA34 ->
 * FUN_00455184 -> FUN_00453D89 once the browser's current-page string at +0x2C0 is non-empty).
 * With the option off the page stays loaded, exactly as the browser control keeps it. */
void html_close(void)
{
    if (!html.open) return;
    wos_log_event("html_close", "url=%s stop=%d", html.url, options_get(HTML_OPTION_STOP_ON_RETURN));
    html.open = 0;
    if (options_get(HTML_OPTION_STOP_ON_RETURN)) html_free();
}

static int dir_of(const char *path, char *out, size_t cap)
{
    const char *slash = strrchr(path, '/');
    size_t n;
    if (!slash) return -1;
    n = (size_t)(slash - path);
    if (n + 1 > cap) return -1;
    memcpy(out, path, n);
    out[n] = 0;
    return 0;
}

/* Load `path` (a platform path) and display it as `display`. Shared by html_open() and by
 * following a link. */
static int load_page(const char *path, const char *display)
{
    size_t size = 0;
    html.text = text_read_file(path, &size);
    if (!html.text) return 0;
    snprintf(html.url, sizeof html.url, "%s", display);
    snprintf(html.page, sizeof html.page, "%s", path);
    if (dir_of(path, html.dir, sizeof html.dir)) html.dir[0] = 0;
    parse();
    html.open = 1;
    wos_log_event("html_open", "url=%s bytes=%u", html.url, (unsigned)size);
    return 1;
}

int html_open(const char *arg)
{
    char path[HTML_PATH_MAX], display[HTML_URL_MAX];
    size_t n;
    if (!arg || !*arg) return 0;
    /* FUN_00467312(0x16), VA 0x48a6e4: preference option 22, "Show HTML pages in scenes, when
     * scripted." Zero means the opcode does nothing at all and the script just advances. */
    if (!options_get(HTML_OPTION_SHOW_IN_SCENES)) {
        wos_log_event("html_disabled", "arg=%s option=%d", arg, HTML_OPTION_SHOW_IN_SCENES);
        return 0;
    }
    html_close();
    if (!strncmp(arg, "http://", 7)) {
        /* The original hands an http:// target straight to the browser without checking that
         * anything exists; the port has no network, so the viewer opens empty. */
        snprintf(html.url, sizeof html.url, "%s", arg);
        html.text = calloc(1, 1);
        if (!html.text) { html_free(); return 0; }
        (void)new_line(HL_TEXT);
        html.content_height = line_height(&html.lines[0]);
        html.open = 1;
        wos_log_event("html_open", "url=%s remote=1", html.url);
        return 1;
    }
    /* FUN_0048A69E builds "%s\worlds\%s\HTML\%s" and prefixes "file:///" (VA 0x48a6f1 and
     * 0x48a761, format strings at 0x502ba8 and 0x502b9c). The port keeps the same shape and the
     * same failure test: the file has to exist, or the opcode simply advances. */
    n = (size_t)snprintf(display, sizeof display, "file:///%s/worlds/%s/HTML/%s",
                         game_data_path(), g_world.name, arg);
    if (n >= sizeof display) return 0;
    if (!world_path(path, sizeof path, "HTML")) return 0;
    n = strlen(path);
    if (n + strlen(arg) + 2 >= sizeof path) return 0;
    snprintf(path + n, sizeof path - n, "/%s", arg);
    /* With option 23 off the page stays loaded after a close, so re-entering it is instant. */
    if (html.text && !strcmp(html.url, display)) {
        html.scroll = 0;
        html.open = 1;
        wos_log_event("html_open", "url=%s retained=1", html.url);
        return 1;
    }
    html_free();
    if (!load_page(path, display)) {
        wos_log_event("html_missing", "url=%s", display);
        return 0;
    }
    return 1;
}

int html_active(void) { return html.open; }
const char *html_url(void) { return html.open ? html.url : ""; }

/* Follow `href` from the current page. Relative targets stay inside the world's HTML folder,
 * which is all the shipped pages link to; a remote target cannot be fetched offline. */
static void follow(const char *href)
{
    char path[HTML_PATH_MAX], dir[HTML_PATH_MAX], display[HTML_PATH_MAX + 16];
    FILE *file;
    if (strstr(href, "://")) { wos_log_event("html_link", "href=%s ok=0", href); return; }
    if (join_path(path, sizeof path, html.dir, href)) {
        wos_log_event("html_link", "href=%s ok=0", href);
        return;
    }
    if (dir_of(path, dir, sizeof dir) || strstr(dir, "..")) {
        wos_log_event("html_link", "href=%s ok=0", href);
        return;
    }
    file = plat_fopen(path, "rb");
    if (!file) { wos_log_event("html_link", "href=%s ok=0", href); return; }
    fclose(file);
    wos_log_event("html_link", "href=%s ok=1", href);
    /* The browser control shows the resolved absolute URL, which is "file:///" + the path. */
    snprintf(display, sizeof display, "file:///%s", path[0] == '/' ? path + 1 : path);
    html_free();
    (void)load_page(path, display);
}


/* --- input and drawing ------------------------------------------------------ */

static int hit(const Input *in, Rect r)
{
    return (in->mouse_pressed & 2u) && in->mouse_x >= r.x && in->mouse_y >= r.y &&
           in->mouse_x < r.x + r.w && in->mouse_y < r.y + r.h;
}

void html_update(const Input *input)
{
    int i, y, hit_line = -1;
    if (!html.open) return;
    if (input->pressed[PLAT_KEY_ESCAPE] || hit(input, close_rect)) { html_close(); return; }
    if (input->pressed[PLAT_KEY_DOWN]) html.scroll += FONT_H * 3;
    if (input->pressed[PLAT_KEY_UP]) html.scroll -= FONT_H * 3;
    if (input->pressed[PLAT_KEY_PAGEDOWN]) html.scroll += PLAT_SCREEN_H - BAR_HEIGHT;
    if (input->pressed[PLAT_KEY_PAGEUP]) html.scroll -= PLAT_SCREEN_H - BAR_HEIGHT;
    if (input->pressed[PLAT_KEY_HOME]) html.scroll = 0;
    if (input->pressed[PLAT_KEY_END]) html.scroll = html.content_height;
    if (input->wheel) html.scroll -= input->wheel * FONT_H * 3;
    if (html.scroll > html.content_height) html.scroll = html.content_height;
    if (html.scroll < 0) html.scroll = 0;
    if (!(input->mouse_pressed & 2u)) return;
    y = BAR_HEIGHT - html.scroll;
    for (i = 0; i < html.line_count; ++i) {
        int h = line_height(&html.lines[i]);
        if (input->mouse_y >= y && input->mouse_y < y + h) { hit_line = i; break; }
        y += h;
    }
    if (hit_line >= 0 && html.lines[hit_line].kind == HL_TEXT && html.lines[hit_line].link >= 0)
        follow(html.links[html.lines[hit_line].link]);
}

static void draw_text(Framebuffer *fb, int x, int y, const char *text, uint32_t color, int scale)
{
    int i;
    if (scale == 1) { font_draw(fb, x, y, text, color); return; }
    for (i = 0; text[i]; ++i) {
        char glyph[2];
        glyph[0] = text[i];
        glyph[1] = 0;
        font_draw(fb, x + i * FONT_W * scale, y, glyph, color);
    }
}

void html_render(Framebuffer *fb)
{
    Rect clip = fb->clip;
    int i, y;
    if (!html.open) return;
    fb_reset_clip(fb);
    fb_fill(fb, (Rect){0, 0, PLAT_SCREEN_W, PLAT_SCREEN_H}, PAGE_COLOR);
    /* The original keeps the URL in a combo box above the browser control. */
    fb_fill(fb, (Rect){0, 0, PLAT_SCREEN_W, BAR_HEIGHT}, BAR_COLOR);
    fb_fill(fb, (Rect){0, BAR_HEIGHT - 1, PLAT_SCREEN_W, 1}, 0x00808080u);
    fb_clip(fb, (Rect){4, 0, close_rect.x - 6, BAR_HEIGHT});
    font_draw(fb, 4, (BAR_HEIGHT - FONT_H) / 2, html.url, BAR_TEXT);
    fb_reset_clip(fb);
    fb_fill(fb, close_rect, CLOSE_COLOR);
    fb_rect(fb, close_rect, 0x00804040u);
    font_draw(fb, close_rect.x + 4, close_rect.y + 4, "Close", 0x00ffffffu);

    fb_clip(fb, (Rect){0, BAR_HEIGHT, PLAT_SCREEN_W, PLAT_SCREEN_H - BAR_HEIGHT});
    y = BAR_HEIGHT - html.scroll;
    for (i = 0; i < html.line_count; ++i) {
        const HtmlLine *line = &html.lines[i];
        int h = line_height(line);
        if (y + h >= BAR_HEIGHT && y < PLAT_SCREEN_H) {
            if (line->kind == HL_IMAGE) {
                const HtmlImage *slot = &html.images[line->image];
                int w = slot->w;
                if (slot->image.pixels && w > 0 && slot->h > 0) {
                    if (w > PLAT_SCREEN_W - 2 * PAGE_MARGIN) w = PLAT_SCREEN_W - 2 * PAGE_MARGIN;
                    fb_blit_sub(fb, &slot->image, (Rect){0, 0, w, slot->h},
                                (line->style & HS_CENTER) ? (PLAT_SCREEN_W - w) / 2 : PAGE_MARGIN,
                                y, 0, -1);
                } else {
                    font_draw(fb, PAGE_MARGIN, y, "[image]", 0x00808080u);
                }
            } else if (line->style & HS_RULE) {
                fb_fill(fb, (Rect){PAGE_MARGIN, y + h / 2, PLAT_SCREEN_W - 2 * PAGE_MARGIN, 1}, 0x00c0c0c0u);
            } else if (line->text[0]) {
                int scale = (line->style & 0x07) ? 2 : 1;
                int tw = font_width(line->text) * scale;
                int tx = (line->style & HS_CENTER) ? (PLAT_SCREEN_W - tw) / 2
                                                   : PAGE_MARGIN + line->indent * FONT_W;
                uint32_t color = line->link >= 0 ? LINK_COLOR
                                 : (line->style & 0x07) ? HEAD_COLOR : BODY_COLOR;
                draw_text(fb, tx, y, line->text, color, scale);
                if (line->link >= 0)
                    fb_fill(fb, (Rect){tx, y + FONT_H * scale, tw, 1}, LINK_COLOR);
            }
        }
        y += h;
    }
    fb->clip = clip;
}

void html_dump(DumpEmit emit, void *user)
{
    char key[32], value[HTML_URL_MAX];
    int i;
    emit("html.url", html.open ? html.url : "", user);
    snprintf(value, sizeof value, "%d", html.open);       emit("html.active", value, user);
    snprintf(value, sizeof value, "%d", html.line_count);  emit("html.lines", value, user);
    snprintf(value, sizeof value, "%d", html.image_count); emit("html.images", value, user);
    snprintf(value, sizeof value, "%d", html.link_count);  emit("html.links", value, user);
    snprintf(value, sizeof value, "%d", html.scroll);      emit("html.scroll", value, user);
    for (i = 0; i < html.link_count; ++i) {
        snprintf(key, sizeof key, "html.link%d", i);
        emit(key, html.links[i], user);
    }
}
