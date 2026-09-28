/* World/shared MIDI lookup. Transcribed from FUN_00468F4F (0x468F4F), the function every
 * "play this music" path funnels into: FUN_00436FC1 (0x436FC1) -> FUN_00468A9A (0x468A9A)
 * -> here. docs/re/maps.md section 6 and Evergreen/music.ini document the playlist.
 *
 * The original stores two flags before doing anything: DAT_004f4158 = param_2 (loop) and
 * DAT_004f415c = param_3 (advance the playlist when the track ends). Map music passes
 * (1, 1) and fight music (1, 0) from FUN_00436FFC (0x436FFC) / FUN_004370F5 (0x4370F5).
 * The port does not loop a track: mapview.c owns DAT_004e70b8 and advances the playlist
 * itself when plat_music_playing() drops, which is what the MCI notify thread
 * (all.c:76196) does in the original. So loop is always 0 here and the flag lives there. */
#include "game.h"
#include "../engine/log.h"
#include "../engine/text.h"
#include "../platform/platform.h"
#include <stdio.h>
#include <string.h>

/* FUN_00468F4F: `_strlwr(name)`, append ".mid" when the name does not already contain it,
 * then truncate at the LAST '.' and append ".mid" again (FUN_00468A5A). Net effect: take
 * the name up to its last dot and give it a .mid extension. ".MID" from the "%s.MID"
 * default in FUN_00436FFC therefore lands as "<root>.mid". */
static void music_leaf(char *out, size_t size, const char *name)
{
    const char *dot;
    size_t n;
    snprintf(out, size, "%s", name);
    dot = strrchr(out, '.');
    if (dot) out[dot - out] = '\0';
    n = strlen(out);
    snprintf(out + n, size - n, ".mid");
}

void game_music(const char *midi_name)
{
    char leaf[264], name[280], relative[320], path[1024];
    FILE *file;
    if (!midi_name || !*midi_name) {
        plat_music_stop();
        wos_log_event("music_stop", NULL);
        return;
    }
    if (strchr(midi_name, '/') || strchr(midi_name, '\\') || strchr(midi_name, ':')) {
        wos_log_event("music_error", "reason=filename");
        return;
    }
    music_leaf(leaf, sizeof(leaf), midi_name);
    /* "%s\\worlds\\%s\\MIDI\\%s" (0x4F4998) then "%s\\MIDI\\%s" (0x4F498C): the world's
     * own MIDI folder first, the shared WoS\MIDI folder second. */
    snprintf(relative, sizeof(relative), "MIDI/%s", leaf);
    file = plat_fopen(world_path(path, sizeof(path), relative), "rb");
    if (!file) file = plat_fopen(world_data_path(path, sizeof(path), relative), "rb");
    if (!file) {
        plat_music_stop();
        wos_log_event("music_error", "file=%s reason=missing", leaf);
        return;
    }
    fclose(file);
    snprintf(name, sizeof(name), "%s", leaf);
    /* DAT_004f4158 = loop would be 1 here; the port's playlist advance lives in mapview.c. */
    plat_music_play(path, 0);
    wos_log_event("music_play", "file=%s", name);
}
