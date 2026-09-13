#ifndef BYPO_AUDIO_H
#define BYPO_AUDIO_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__linux__)
#include <alsa/asoundlib.h>
#endif

typedef void (*AudioRender)(void *userdata, float *interleaved, size_t frames,
                            int channels);

typedef struct {
#if defined(__linux__)
    snd_pcm_t *pcm;
    pthread_t thread;
#else
    uint32_t dev; /* SDL_AudioDeviceID; 0 when closed */
#endif
    unsigned rate;
    int channels;
    _Atomic bool running;
    AudioRender render;
    void *userdata;
} AudioOut;

/* opens the default device (float, stereo preferred); fills rate/channels */
int audio_out_open(AudioOut *a);
int audio_out_start(AudioOut *a, AudioRender render, void *userdata);
void audio_out_stop(AudioOut *a);

#endif
