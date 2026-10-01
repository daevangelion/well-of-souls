#include "scenecache.h"
#include "options.h"
#include "../engine/clock.h"
#include <stdio.h>
#include <string.h>

#define SCENE_CACHE_OPTION 30      /* FUN_00467312(0x1e) */
#define SCENE_CACHE_SLOTS 256

static char cached[SCENE_CACHE_SLOTS][300];
static int cached_count;

void scene_cache_load(int w, int h, const char *path)
{
    char key[sizeof(cached[0])];
    int size = h > w ? h : w, i;
    if (size < 1 || !path || !*path) return;
    if (!options_get(SCENE_CACHE_OPTION) || !strstr(path, ".jpg")) return;
    snprintf(key, sizeof(key), "%d_%s", size, path);
    for (i = 0; i < cached_count; ++i) if (!strcmp(cached[i], key)) return;  /* _access hit */
    if (cached_count < SCENE_CACHE_SLOTS) snprintf(cached[cached_count++], sizeof(cached[0]), "%s", key);
    clock_stall(50 + 100);         /* FUN_0048A05F: Sleep(50), the wait, Sleep(100) */
}
