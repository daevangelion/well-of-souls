#include "replay.h"
#include "text.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
static char *token(char **cursor)
{
    char *s=*cursor,*e;
    while(isspace((unsigned char)*s)) ++s;
    if(!*s) { *cursor=s; return NULL; }
    e=s; while(*e && !isspace((unsigned char)*e)) ++e;
    if(*e) *e++=0;
    *cursor=e; return s;
}
static int number(const char *s,int allow_negative,int64_t *out)
{
    char *end; long long n;
    if(!s || !*s || (!allow_negative && *s=='-')) return -1;
    errno=0; n=strtoll(s,&end,10);
    if(errno || *end || n<INT_MIN || n>UINT32_MAX) return -1;
    *out=n; return 0;
}
int replay_key(const char *name)
{
    static const struct { const char *name; int key; } keys[]={
        {"RETURN",PLAT_KEY_RETURN},{"ESCAPE",PLAT_KEY_ESCAPE},{"TAB",PLAT_KEY_TAB},
        {"BACKSPACE",PLAT_KEY_BACKSPACE},{"SPACE",PLAT_KEY_SPACE},{"UP",PLAT_KEY_UP},
        {"DOWN",PLAT_KEY_DOWN},{"LEFT",PLAT_KEY_LEFT},{"RIGHT",PLAT_KEY_RIGHT}
    };
    size_t i;
    if(!name || !*name) return -1;
    if(!name[1] && (unsigned char)name[0]>=33 && (unsigned char)name[0]<=126) return tolower((unsigned char)name[0]);
    for(i=0;i<sizeof(keys)/sizeof(keys[0]);++i) if(!text_casecmp(name,keys[i].name)) return keys[i].key;
    if(name[0]=='F' || name[0]=='f') {
        int64_t n; if(!number(name+1,0,&n) && n>=1 && n<=12) return PLAT_KEY_F1+(int)n-1;
    }
    return -1;
}
int replay_parse(Replay *rp,char *text,size_t *error_line)
{
    char *cursor=text,*line; size_t line_no=0;
    memset(rp,0,sizeof(*rp)); if(error_line) *error_line=0;
    while((line=text_next_line(&cursor))) {
        ReplayCommand c={0}; char *cmd,*args,*a; int64_t n;
        ++line_no; while(isspace((unsigned char)*line)) ++line;
        if(!*line || *line=='#') continue;
        args=line; cmd=token(&args);
        if(!strcmp(cmd,"text")) {
            while(*args==' ' || *args=='\t') ++args;
            if(strlen(args)>1023) goto fail;
            c.op=RP_TEXT; c.text=args;
        } else {
            if(!strcmp(cmd,"wait")) {
                c.op=RP_WAIT; if(number(token(&args),0,&n)) goto fail; c.frames=(uint32_t)n;
            } else if(!strcmp(cmd,"key") || !strcmp(cmd,"keydown") || !strcmp(cmd,"keyup") || !strcmp(cmd,"hold")) {
                c.op=!strcmp(cmd,"key")?RP_KEY:!strcmp(cmd,"keydown")?RP_DOWN:!strcmp(cmd,"keyup")?RP_UP:RP_HOLD;
                c.key=replay_key(token(&args)); if(c.key<0) goto fail;
                c.frames=1;
                if(c.op==RP_HOLD) { if(number(token(&args),0,&n)) goto fail; c.frames=(uint32_t)n; }
            } else if(!strcmp(cmd,"move") || !strcmp(cmd,"click") || !strcmp(cmd,"rclick")) {
                c.op=!strcmp(cmd,"move")?RP_MOVE:RP_CLICK;
                if(number(token(&args),1,&n) || n>INT_MAX) goto fail;
                c.x=(int)n;
                if(number(token(&args),1,&n) || n>INT_MAX) goto fail;
                c.y=(int)n; c.button=!strcmp(cmd,"rclick")?3:1;
                if(!strcmp(cmd,"click") && (a=token(&args))) {
                    if(number(a,0,&n) || n<1 || n>3) goto fail;
                    c.button=(int)n;
                }
            } else if(!strcmp(cmd,"expect")) {
                c.op=RP_EXPECT; c.text=token(&args); c.frames=600;
                if(!c.text) goto fail;
                if((a=token(&args))) { if(number(a,0,&n)) goto fail; c.frames=(uint32_t)n; }
            } else if(!strcmp(cmd,"quit")) c.op=RP_QUIT;
            else goto fail;
            if(token(&args)) goto fail;
        }
        if(rp->count==REPLAY_COMMAND_MAX) goto fail;
        rp->commands[rp->count++]=c;
    }
    return 0;
fail:
    if(error_line) *error_line=line_no;
    rp->count=0; return -1;
}
int replay_step(Replay *rp,PlatEvent events[REPLAY_EVENTS_MAX],size_t *count,uint64_t serial,ReplaySeen seen,void *user)
{
    *count=0;
    if(rp->remaining) { --rp->remaining; return 0; }
    if(rp->pending.type!=PLAT_EV_NONE) {
        events[(*count)++]=rp->pending; memset(&rp->pending,0,sizeof(rp->pending)); return 0;
    }
    while(rp->pc<rp->count) {
        ReplayCommand *c=&rp->commands[rp->pc]; PlatEvent ev={0};
        if(c->op==RP_EXPECT) {
            if(seen && seen(c->text,rp->observed,user)) { rp->observed=serial; rp->expect_elapsed=0; ++rp->pc; continue; }
            if(rp->expect_elapsed>=c->frames) { rp->failed_event=c->text; return 2; }
            ++rp->expect_elapsed; return 0;
        }
        ++rp->pc;
        switch(c->op) {
        case RP_WAIT:
            if(!c->frames) continue;
            rp->remaining=c->frames-1; return 0;
        case RP_QUIT: return 1;
        case RP_KEY: case RP_DOWN: case RP_UP: case RP_HOLD:
            ev.type=c->op==RP_UP?PLAT_EV_KEY_UP:PLAT_EV_KEY_DOWN; ev.key=c->key;
            events[(*count)++]=ev;
            if(c->op==RP_KEY || c->op==RP_HOLD) {
                ev.type=PLAT_EV_KEY_UP; rp->pending=ev; rp->remaining=c->frames?c->frames-1:0;
                if(c->op==RP_HOLD && !c->frames) { events[(*count)++]=ev; memset(&rp->pending,0,sizeof(rp->pending)); }
            }
            return 0;
        case RP_CLICK:
            ev.type=PLAT_EV_MOUSE_MOVE; ev.x=c->x; ev.y=c->y; events[(*count)++]=ev;
            ev.type=PLAT_EV_MOUSE_DOWN; ev.button=c->button; events[(*count)++]=ev;
            ev.type=PLAT_EV_MOUSE_UP; rp->pending=ev; return 0;
        case RP_MOVE:
            ev.type=PLAT_EV_MOUSE_MOVE; ev.x=c->x; ev.y=c->y; events[(*count)++]=ev; return 0;
        case RP_TEXT: {
            const char *s=c->text;
            while(*s) {
                size_t n=strlen(s); if(n>31) n=31;
                /* Do not split UTF-8 continuation bytes across platform events. */
                while(n>28 && s[n] && ((unsigned char)s[n]&0xc0)==0x80) --n;
                memset(&ev,0,sizeof(ev)); ev.type=PLAT_EV_TEXT; memcpy(ev.text,s,n); s+=n;
                events[(*count)++]=ev;
            }
            return 0;
        }
        default: break;
        }
    }
    return 1;
}
