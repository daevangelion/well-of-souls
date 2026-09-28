#include "dscript.h"
#include "replay.h" /* replay_key(): the .rpl and .dsc key vocabularies are one */
#include "text.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static char *token(char **cursor)
{
    char *s = *cursor, *e;
    while (isspace((unsigned char)*s)) ++s;
    if (!*s) { *cursor = s; return NULL; }
    e = s;
    while (*e && !isspace((unsigned char)*e)) ++e;
    if (*e) *e++ = 0;
    *cursor = e;
    return s;
}

static int number(const char *s, int64_t *out)
{
    char *end;
    long long n;
    if (!s || !*s || *s == '-') return -1;
    errno = 0;
    n = strtoll(s, &end, 10);
    if (errno || *end || n < 0 || n > UINT32_MAX) return -1;
    *out = n;
    return 0;
}

/* `ctrl=value`, already split out of the line as one whitespace-free token. */
static int parse_pair(DscriptOp *c, const char *tok)
{
    char buf[64], *eq;
    int64_t n;
    size_t len = strlen(tok);
    if (!len || len >= sizeof(buf) || c->control_count == DSCRIPT_KV_MAX) return -1;
    memcpy(buf, tok, len + 1);
    eq = strchr(buf, '=');
    if (!eq) return -1;
    *eq = 0;
    if (number(buf, &n)) return -1;
    c->control[c->control_count] = (int)n;
    if (number(eq + 1, &n)) return -1;
    c->value[c->control_count] = (int)n;
    ++c->control_count;
    return 0;
}

static int parse_pointer(DscriptOp *c, char **cursor)
{
    char *t = token(cursor);
    int64_t n;
    if (!t || number(t, &n) || n > INT_MAX) return -1;
    c->x = (int)n;
    t = token(cursor);
    if (!t || number(t, &n) || n > INT_MAX) return -1;
    c->y = (int)n;
    t = token(cursor);
    if (t) {
        if (number(t, &n) || n < 1 || n > 3) return -1;
        c->button = (int)n;
    } else if (c->kind == DS_MOVE) {
        c->button = 0; /* a bare move carries no button */
    }
    return token(cursor) ? -1 : 0;
}

int dscript_parse(Dscript *ds, char *text, size_t *error_line)
{
    char *cursor = text, *line;
    size_t line_no = 0;
    memset(ds, 0, sizeof(*ds));
    if (error_line) *error_line = 0;
    while ((line = text_next_line(&cursor))) {
        DscriptOp c;
        char *args, *verb, *t;
        int64_t n;
        memset(&c, 0, sizeof(c));
        c.button = 1;
        ++line_no;
        while (isspace((unsigned char)*line)) ++line;
        if (!*line || *line == '#') continue;
        args = line;
        verb = token(&args);
        if (!verb) continue;
        if (!strcmp(verb, "end")) {
            if (number(token(&args), &n) || token(&args)) goto bad;
            ds->end_ms = (uint32_t)n;
            ds->has_end = 1;
            continue;
        }
        if (strcmp(verb, "at")) goto bad;
        if (number(token(&args), &n)) goto bad;
        c.at_ms = (uint32_t)n;
        verb = token(&args);
        if (!verb) goto bad;

        if (!strcmp(verb, "click") || !strcmp(verb, "rclick") || !strcmp(verb, "down") ||
            !strcmp(verb, "up") || !strcmp(verb, "move")) {
            c.kind = !strcmp(verb, "move") ? DS_MOVE
                   : !strcmp(verb, "click") ? DS_CLICK
                   : !strcmp(verb, "rclick") ? DS_RCLICK
                   : !strcmp(verb, "down") ? DS_DOWN : DS_UP;
            c.button = c.kind == DS_RCLICK ? 3 : (c.kind == DS_MOVE ? 0 : 1);
            if (parse_pointer(&c, &args)) goto bad;
        } else if (!strcmp(verb, "key")) {
            t = token(&args);
            c.kind = DS_KEY;
            if (!t || (c.key = replay_key(t)) < 0 || token(&args)) goto bad;
        } else if (!strcmp(verb, "text")) {
            c.kind = DS_TEXT;
            while (*args == ' ' || *args == '\t') ++args;
            if (strlen(args) >= DSCRIPT_TEXT_MAX) goto bad;
            memcpy(c.text, args, strlen(args) + 1);
        } else if (!strcmp(verb, "dialog")) {
            c.kind = DS_DIALOG;
            t = token(&args);
            if (!t || number(t, &n)) goto bad;
            c.id = (int)n;
            for (;;) {
                t = token(&args);
                if (!t) goto bad;
                if (!strcmp(t, "ok")) { c.ok = 1; break; }
                if (!strcmp(t, "cancel")) { c.ok = 0; break; }
                if (parse_pair(&c, t)) goto bad;
            }
            if (token(&args)) goto bad;
        } else if (!strcmp(verb, "dump")) {
            t = token(&args);
            c.kind = DS_DUMP;
            if (!t || strlen(t) >= sizeof(c.text) || token(&args)) goto bad;
            memcpy(c.text, t, strlen(t) + 1);
        } else {
            goto bad;
        }
        if (ds->count == DSCRIPT_OPS_MAX) goto bad;
        ds->ops[ds->count++] = c;
    }
    return 0;
bad:
    if (error_line) *error_line = line_no;
    ds->count = 0;
    return -1;
}

uint32_t dscript_next_time(const Dscript *ds, uint32_t from)
{
    uint32_t best = UINT32_MAX;
    size_t i;
    for (i = ds->pc; i < ds->count; ++i) {
        if (ds->ops[i].at_ms < from) continue;
        if (ds->ops[i].at_ms < best) best = ds->ops[i].at_ms;
    }
    if (ds->has_end && ds->end_ms >= from && ds->end_ms < best) best = ds->end_ms;
    return best;
}

const DscriptOp *dscript_take(Dscript *ds, uint32_t now_ms)
{
    if (ds->pc >= ds->count || ds->ops[ds->pc].at_ms > now_ms) return NULL;
    return &ds->ops[ds->pc++];
}

