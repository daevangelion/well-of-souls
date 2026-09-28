#include "platform/platform.h"
#include "third_party/tsf.h"
#include "third_party/tml.h"
#include <SDL.h>
#include <SDL_system.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#define SOUND_CHANNELS 8
#define AUDIO_RATE 44100
#define MIX_FRAMES 256
#define MIDI_MAX (16 * 1024 * 1024)
static struct { Uint8 *data; Uint32 len, pos; } sounds[SOUND_CHANNELS];
static SDL_AudioDeviceID device;
static unsigned next_channel;
static tsf *synth;
static tml_message *song, *event;
static uint64_t music_frame;
static int music_loop, music_active, music_released;

static int sf_read(void *data, void *out, unsigned size)
{ return (int)SDL_RWread(data, out, 1, size); }
static int sf_skip(void *data, unsigned size)
{ return SDL_RWseek(data, size, RW_SEEK_CUR) >= 0; }

static tsf *load_soundfont(const char *path, int asset)
{
    SDL_RWops *rw;
    struct tsf_stream stream;
    tsf *font;
    if (asset) rw = SDL_RWFromFile(path, "rb");
    else {
        FILE *file = plat_fopen(path, "rb");
        if (!file) return NULL;
        rw = SDL_RWFromFP(file, SDL_TRUE);
        if (!rw) { fclose(file); return NULL; }
    }
    if (!rw) return NULL;
    stream.data = rw; stream.read = sf_read; stream.skip = sf_skip;
    font = tsf_load(&stream);
    SDL_RWclose(rw);
    return font;
}

static tsf *find_soundfont(void)
{
    const char *env = SDL_getenv("WOS_SOUNDFONT"), *home;
    char path[4096], *base;
    tsf *font = NULL;
    int n;
    if (env && *env) font = load_soundfont(env, 0);
    if (font) return font;
    home = SDL_getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = SDL_getenv("USERPROFILE");
#endif
    /* A user-supplied bank (written by the app's supply flow) takes precedence over
     * the bundled/default one. On Android SDL never sets HOME to the app's internal
     * storage, so check SDL_AndroidGetInternalStoragePath() (== getFilesDir()) directly. */
#ifdef __ANDROID__
    {
        const char *adir = SDL_AndroidGetInternalStoragePath();
        if (adir && *adir) {
            n = SDL_snprintf(path, sizeof(path), "%s/user-soundfont.sf2", adir);
            if (n >= 0 && n < (int)sizeof(path)) font = load_soundfont(path, 0);
            if (font) return font;
        }
    }
#endif
    if (home && *home) {
        n = SDL_snprintf(path, sizeof(path), "%s/user-soundfont.sf2", home);
        if (n >= 0 && n < (int)sizeof(path)) font = load_soundfont(path, 0);
        if (font) return font;
    }
    base = SDL_GetBasePath();
    if (base) {
        n = SDL_snprintf(path, sizeof(path), "%sTimGM6mb.sf2", base);
        if (n >= 0 && n < (int)sizeof(path)) font = load_soundfont(path, 0);
        SDL_free(base);
    }
    if (font) return font;
#ifdef __ANDROID__
    font = load_soundfont("TimGM6mb.sf2", 1);
    if (font) return font;
#endif
    if (home && *home) {
        n = SDL_snprintf(path, sizeof(path), "%s/.cache/wos-soundfont/TimGM6mb.sf2", home);
        if (n >= 0 && n < (int)sizeof(path)) font = load_soundfont(path, 0);
    }
    return font;
}

/* All sixteen channels and 256 voices are allocated before the device starts. */
static void reset_channels(void)
{
    int i;
    for (i = 0; i < 16; ++i) {
        tsf_channel_sounds_off_all(synth, i);
        tsf_channel_midi_control(synth, i, 121, 0);
        tsf_channel_set_pitchwheel(synth, i, 8192);
        tsf_channel_set_sustain(synth, i, 0);
        tsf_channel_set_presetnumber(synth, i, 0, i == 9);
    }
}

static void midi_event(const tml_message *msg)
{
    switch (msg->type) {
    case TML_PROGRAM_CHANGE: tsf_channel_set_presetnumber(synth, msg->channel, msg->program, msg->channel == 9); break;
    case TML_NOTE_ON: tsf_channel_note_on(synth, msg->channel, msg->key, msg->velocity / 127.0f); break;
    case TML_NOTE_OFF: tsf_channel_note_off(synth, msg->channel, msg->key); break;
    case TML_PITCH_BEND: tsf_channel_set_pitchwheel(synth, msg->channel, msg->pitch_bend); break;
    case TML_CONTROL_CHANGE: tsf_channel_midi_control(synth, msg->channel, msg->control, msg->control_value); break;
    default: break; /* TML already applied tempo to message timestamps. */
    }
}

static void render_music(float *out, int frames)
{
    int done = 0;
    memset(out, 0, (size_t)frames * 2 * sizeof(*out));
    while (music_active && done < frames) {
        int count = frames - done;
        while (event && (uint64_t)event->time * AUDIO_RATE <= music_frame * 1000) {
            midi_event(event);
            event = event->next;
        }
        if (!event && !music_released) {
            /* Release even malformed songs that omit their final note-offs/sustain-up. */
            tsf_note_off_all(synth);
            music_released = 1;
        }
        if (!event && !tsf_active_voice_count(synth)) {
            if (!music_loop || !music_frame) { music_active = 0; break; }
            reset_channels();
            event = song; music_frame = 0; music_released = 0;
            continue;
        }
        if (event) {
            uint64_t next_frame = ((uint64_t)event->time * AUDIO_RATE + 999) / 1000;
            if (next_frame - music_frame < (uint64_t)count) count = (int)(next_frame - music_frame);
        }
        tsf_render_float(synth, out + done * 2, count, 0);
        music_frame += (unsigned)count;
        done += count;
    }
    if (!event && music_released && !music_loop && synth && !tsf_active_voice_count(synth)) music_active = 0;
}

static void mix_sounds(void *unused, Uint8 *stream, int len)
{
    float mix[MIX_FRAMES * 2];
    Sint16 *output = (Sint16 *)stream;
    int remaining = len / (2 * (int)sizeof(*output));
    (void)unused;
    memset(stream, 0, (size_t)len);
    while (remaining > 0) {
        int frames = remaining > MIX_FRAMES ? MIX_FRAMES : remaining;
        int samples = frames * 2, i, ch;
        render_music(mix, frames);
        for (ch = 0; ch < SOUND_CHANNELS; ++ch) {
            Uint32 count = (sounds[ch].len - sounds[ch].pos) / sizeof(Sint16);
            const Sint16 *wav;
            if (!count) continue;
            wav = (const Sint16 *)(sounds[ch].data + sounds[ch].pos);
            if (count > (Uint32)samples) count = (Uint32)samples;
            for (i = 0; i < (int)count; ++i) mix[i] += wav[i] / 65536.0f;
            sounds[ch].pos += count * sizeof(Sint16);
        }
        /* One smooth limiter after summing: headroom without integer clipping/wrap. */
        for (i = 0; i < samples; ++i) output[i] = (Sint16)(mix[i] / (1.0f + fabsf(mix[i])) * 32767.0f);
        output += samples;
        remaining -= frames;
    }
}

void wos_audio_init(void)
{
    SDL_AudioSpec want;
    static int warned;
    if (device || SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) return;
    synth = find_soundfont();
    if (synth) {
        tsf_set_output(synth, TSF_STEREO_INTERLEAVED, AUDIO_RATE, -10.0f);
        if (!tsf_set_max_voices(synth, 256) || !tsf_channel_set_presetnumber(synth, 15, 0, 0)) {
            tsf_close(synth); synth = NULL;
        } else reset_channels();
    }
    if (!synth && !warned) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No usable soundfont; MIDI disabled. Run tools/fetch_soundfont.sh or set WOS_SOUNDFONT.");
        warned = 1;
    }
    SDL_zero(want);
    want.freq = AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2;
    want.samples = 1024; want.callback = mix_sounds;
    device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!device) { if (synth) tsf_close(synth); synth = NULL; return; }
    SDL_PauseAudioDevice(device, 0);
}

void plat_music_stop(void)
{
    tml_message *old;
    if (!device) return;
    SDL_LockAudioDevice(device);
    old = song; song = event = NULL;
    music_active = music_loop = music_released = 0; music_frame = 0;
    SDL_UnlockAudioDevice(device);
    tml_free(old);
}

int plat_music_playing(void)
{
    int playing;
    if (!device) return 0;
    SDL_LockAudioDevice(device);
    playing = music_active;
    SDL_UnlockAudioDevice(device);
    return playing;
}

void wos_audio_shutdown(void)
{
    int i;
    if (device) SDL_CloseAudioDevice(device);
    device = 0;
    tml_free(song); song = event = NULL;
    if (synth) tsf_close(synth);
    synth = NULL; music_active = music_loop = music_released = 0; music_frame = 0;
    for (i = 0; i < SOUND_CHANNELS; ++i) {
        SDL_free(sounds[i].data);
        memset(&sounds[i], 0, sizeof(sounds[i]));
    }
    next_channel = 0;
}

void plat_sound_play(const void *wav, size_t len)
{
    SDL_AudioSpec source;
    SDL_AudioCVT cvt;
    Uint8 *decoded, *buffer, *old;
    Uint32 decoded_len;
    unsigned i, channel;
    if (!device || !wav || !len || len > INT_MAX) return;
    if (!SDL_LoadWAV_RW(SDL_RWFromConstMem(wav, (int)len), 1, &source, &decoded, &decoded_len)) return;
    if (decoded_len > INT_MAX || SDL_BuildAudioCVT(&cvt, source.format, source.channels,
            source.freq, AUDIO_S16SYS, 2, AUDIO_RATE) < 0 || cvt.len_mult <= 0 ||
            decoded_len > (Uint32)(INT_MAX / cvt.len_mult)) {
        SDL_FreeWAV(decoded); return;
    }
    buffer = SDL_malloc((size_t)decoded_len * (size_t)cvt.len_mult);
    if (!buffer) { SDL_FreeWAV(decoded); return; }
    memcpy(buffer, decoded, decoded_len); SDL_FreeWAV(decoded);
    cvt.buf = buffer; cvt.len = (int)decoded_len;
    if (SDL_ConvertAudio(&cvt) < 0) { SDL_free(buffer); return; }
    SDL_LockAudioDevice(device);
    channel = next_channel;
    for (i = 0; i < SOUND_CHANNELS; ++i) {
        if (sounds[i].pos == sounds[i].len) { channel = i; break; }
    }
    old = sounds[channel].data;
    sounds[channel].data = buffer; sounds[channel].len = (Uint32)cvt.len_cvt; sounds[channel].pos = 0;
    next_channel = (channel + 1) % SOUND_CHANNELS;
    SDL_UnlockAudioDevice(device);
    SDL_free(old);
}

void plat_music_play(const char *midi_path, int loop)
{
    FILE *file;
    long size;
    void *bytes;
    tml_message *replacement, *old;
    float discard[1024 * 2];
    if (!device || !synth || !midi_path) return;
    file = plat_fopen(midi_path, "rb");
    if (!file) return;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) <= 0 || size > MIDI_MAX || fseek(file, 0, SEEK_SET)) {
        fclose(file); return;
    }
    bytes = SDL_malloc((size_t)size);
    if (!bytes) { fclose(file); return; }
    if (fread(bytes, 1, (size_t)size, file) != (size_t)size) { SDL_free(bytes); fclose(file); return; }
    fclose(file);
    replacement = tml_load_memory(bytes, (int)size);
    SDL_free(bytes);
    if (!replacement) return;
    SDL_LockAudioDevice(device);
    reset_channels();
    /* TSF all-sounds-off uses a short release; discard it before the new song. */
    if (tsf_active_voice_count(synth)) tsf_render_float(synth, discard, 1024, 0);
    old = song; song = event = replacement; music_frame = 0;
    music_loop = !!loop; music_released = 0; music_active = 1;
    SDL_UnlockAudioDevice(device);
    tml_free(old);
}
