#include "platform/platform.h"
#include <SDL.h>
#include <limits.h>
#include <string.h>
#ifdef WOS_HAVE_SDL_MIXER
#include <SDL_mixer.h>
#endif

#define SOUND_CHANNELS 8
static struct { Uint8 *data; Uint32 len, pos; } sounds[SOUND_CHANNELS];
static SDL_SpinLock sounds_lock;
static int audio_ready;
static unsigned next_channel;
#ifndef WOS_HAVE_SDL_MIXER
static SDL_AudioDeviceID device;
#else
static Mix_Music *music;
#endif

static void mix_sounds(void *unused, Uint8 *stream, int len)
{
    int i;
    (void)unused;
#ifndef WOS_HAVE_SDL_MIXER
    memset(stream, 0, (size_t)len);
#endif
    SDL_AtomicLock(&sounds_lock);
    for (i = 0; i < SOUND_CHANNELS; ++i) {
        Uint32 n = sounds[i].len - sounds[i].pos;
        if (n > (Uint32)len) n = (Uint32)len;
        if (n) {
            SDL_MixAudioFormat(stream, sounds[i].data + sounds[i].pos, AUDIO_S16SYS,
                               n, SDL_MIX_MAXVOLUME / 2);
            sounds[i].pos += n;
        }
    }
    SDL_AtomicUnlock(&sounds_lock);
}

void wos_audio_init(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) return;
#ifdef WOS_HAVE_SDL_MIXER
    if (Mix_OpenAudioDevice(44100, AUDIO_S16SYS, 2, 1024, NULL, 0) < 0) return;
    Mix_SetPostMix(mix_sounds, NULL);
#else
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix_sounds;
    device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!device) return;
    SDL_PauseAudioDevice(device, 0);
#endif
    audio_ready = 1;
}

void plat_music_stop(void)
{
#ifdef WOS_HAVE_SDL_MIXER
    if (!audio_ready) return;
    Mix_HaltMusic();
    Mix_FreeMusic(music);
    music = NULL;
#endif
}

void wos_audio_shutdown(void)
{
    int i;
#ifdef WOS_HAVE_SDL_MIXER
    if (audio_ready) {
        plat_music_stop();
        Mix_SetPostMix(NULL, NULL);
        Mix_CloseAudio();
    }
#else
    if (device) SDL_CloseAudioDevice(device);
    device = 0;
#endif
    audio_ready = 0;
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
    Uint8 *decoded, *buffer;
    Uint32 decoded_len;
    unsigned i, channel;
    if (!audio_ready || !wav || !len || len > INT_MAX) return;
    if (!SDL_LoadWAV_RW(SDL_RWFromConstMem(wav, (int)len), 1, &source, &decoded, &decoded_len)) return;
    if (decoded_len > INT_MAX || SDL_BuildAudioCVT(&cvt, source.format, source.channels,
            source.freq, AUDIO_S16SYS, 2, 44100) < 0 || cvt.len_mult <= 0 ||
            decoded_len > (Uint32)(INT_MAX / cvt.len_mult)) {
        SDL_FreeWAV(decoded);
        return;
    }
    buffer = SDL_malloc((size_t)decoded_len * (size_t)cvt.len_mult);
    if (!buffer) { SDL_FreeWAV(decoded); return; }
    memcpy(buffer, decoded, decoded_len);
    SDL_FreeWAV(decoded);
    cvt.buf = buffer;
    cvt.len = (int)decoded_len;
    if (SDL_ConvertAudio(&cvt) < 0) { SDL_free(buffer); return; }
    SDL_AtomicLock(&sounds_lock);
    channel = next_channel;
    for (i = 0; i < SOUND_CHANNELS; ++i) {
        if (sounds[i].pos == sounds[i].len) { channel = i; break; }
    }
    SDL_free(sounds[channel].data);
    sounds[channel].data = buffer;
    sounds[channel].len = (Uint32)cvt.len_cvt;
    sounds[channel].pos = 0;
    next_channel = (channel + 1) % SOUND_CHANNELS;
    SDL_AtomicUnlock(&sounds_lock);
}

void plat_music_play(const char *midi_path, int loop)
{
#ifdef WOS_HAVE_SDL_MIXER
    FILE *file;
    SDL_RWops *rw;
    Mix_Music *replacement;
    if (!audio_ready || !midi_path) return;
    file = plat_fopen(midi_path, "rb");
    if (!file) return;
    rw = SDL_RWFromFP(file, SDL_TRUE);
    if (!rw) { fclose(file); return; }
    replacement = Mix_LoadMUS_RW(rw, 1);
    if (!replacement) return;
    plat_music_stop();
    music = replacement;
    if (Mix_PlayMusic(music, loop ? -1 : 0) < 0) plat_music_stop();
#else
    (void)midi_path;
    (void)loop;
#endif
}
