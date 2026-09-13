#include "audio.h"

#include <stdio.h>
#include <string.h>

#define CHUNK_FRAMES 256

#if defined(__linux__)

/* ---------- ALSA: the original backend ---------- */

int audio_out_open(AudioOut *a) {
    a->pcm = NULL;
    a->render = NULL;
    atomic_store(&a->running, false);
    int err = snd_pcm_open(&a->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "audio: %s\n", snd_strerror(err));
        return -1;
    }
    a->rate = 48000;
    a->channels = 2;
    err = snd_pcm_set_params(a->pcm, SND_PCM_FORMAT_FLOAT_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED, a->channels,
                             a->rate, 1, 50000);
    if (err < 0) {
        fprintf(stderr, "audio: %s\n", snd_strerror(err));
        snd_pcm_close(a->pcm);
        a->pcm = NULL;
        return -1;
    }
    return 0;
}

static void *audio_thread(void *arg) {
    AudioOut *a = arg;
    float buf[CHUNK_FRAMES * 8];
    while (atomic_load(&a->running)) {
        a->render(a->userdata, buf, CHUNK_FRAMES, a->channels);
        snd_pcm_sframes_t n = snd_pcm_writei(a->pcm, buf, CHUNK_FRAMES);
        if (n < 0) {
            n = snd_pcm_recover(a->pcm, (int)n, 1);
            if (n < 0) {
                fprintf(stderr, "audio stream error: %s\n", snd_strerror((int)n));
                break;
            }
        }
    }
    return NULL;
}

int audio_out_start(AudioOut *a, AudioRender render, void *userdata) {
    a->render = render;
    a->userdata = userdata;
    atomic_store(&a->running, true);
    if (pthread_create(&a->thread, NULL, audio_thread, a) != 0) {
        atomic_store(&a->running, false);
        return -1;
    }
    return 0;
}

void audio_out_stop(AudioOut *a) {
    if (!a->pcm) return;
    if (atomic_exchange(&a->running, false)) pthread_join(a->thread, NULL);
    snd_pcm_drain(a->pcm);
    snd_pcm_close(a->pcm);
    a->pcm = NULL;
}

#else

/* ---------- SDL2 audio: macOS (CoreAudio) and Windows (WASAPI) ----------
   SDL is already a hard dependency of the GUI, and its audio subsystem
   fronts the native API on both platforms; asking for exactly the format
   the engine wants (48 kHz float stereo) makes SDL convert on our behalf,
   so the engine sees the same stream it does under ALSA. */

#include <SDL2/SDL.h>

static void sdl_callback(void *ud, Uint8 *stream, int len) {
    AudioOut *a = ud;
    size_t frames = (size_t)len / ((size_t)a->channels * sizeof(float));
    if (a->render && atomic_load(&a->running))
        a->render(a->userdata, (float *)stream, frames, a->channels);
    else
        memset(stream, 0, (size_t)len);
}

int audio_out_open(AudioOut *a) {
    a->dev = 0;
    a->render = NULL;
    atomic_store(&a->running, false);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "audio: %s\n", SDL_GetError());
        return -1;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = CHUNK_FRAMES;
    want.callback = sdl_callback;
    want.userdata = a;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0) {
        fprintf(stderr, "audio: %s\n", SDL_GetError());
        return -1;
    }
    a->dev = dev;
    a->rate = (unsigned)have.freq;
    a->channels = have.channels;
    return 0;
}

int audio_out_start(AudioOut *a, AudioRender render, void *userdata) {
    if (!a->dev) return -1;
    a->render = render;
    a->userdata = userdata;
    atomic_store(&a->running, true);
    SDL_PauseAudioDevice((SDL_AudioDeviceID)a->dev, 0);
    return 0;
}

void audio_out_stop(AudioOut *a) {
    if (!a->dev) return;
    atomic_store(&a->running, false);
    SDL_PauseAudioDevice((SDL_AudioDeviceID)a->dev, 1);
    SDL_CloseAudioDevice((SDL_AudioDeviceID)a->dev);
    a->dev = 0;
}

#endif
