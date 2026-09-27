/* World/shared MIDI lookup: Evergreen/music.ini lines 18-23 and 44-46. */
#include "game.h"
#include "../engine/log.h"
#include "../engine/text.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>

void game_music(const char *midi_name)
{
    char name[256], relative[272], path[1024];
    FILE *file;
    size_t len;
    int n;
    if (!midi_name || !*midi_name) {
        plat_music_stop();
        wos_log_event("music_stop", NULL);
        return;
    }
    len = strlen(midi_name);
    if (strchr(midi_name, '/') || strchr(midi_name, '\\') || strchr(midi_name, ':')) {
        wos_log_event("music_error", "reason=filename");
        return;
    }
    n = snprintf(name, sizeof(name), "%s%s", midi_name,
                 len >= 4 && text_casecmp(midi_name + len - 4, ".mid") == 0 ? "" : ".mid");
    if (n < 0 || (size_t)n >= sizeof(name)) {
        wos_log_event("music_error", "reason=filename");
        return;
    }
    snprintf(relative, sizeof(relative), "MIDI/%s", name);
    file = plat_fopen(world_path(path, sizeof(path), relative), "rb");
    if (!file) file = plat_fopen(world_data_path(path, sizeof(path), relative), "rb");
    if (!file) {
        plat_music_stop();
        wos_log_event("music_error", "file=%s reason=missing", name);
        return;
    }
    fclose(file);
    /* Do not force looping: music.ini playlists advance when the track ends. */
    plat_music_play(path, 0);
    wos_log_event("music_play", "file=%s", name);
}
