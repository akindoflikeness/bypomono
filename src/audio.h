#ifndef BYPO_AUDIO_H
#define BYPO_AUDIO_H

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

typedef void (*AudioRender)(void *userdata, float *interleaved, size_t frames,
                            int channels);

typedef struct {
    snd_pcm_t *pcm;
    unsigned rate;
    int channels;
    pthread_t thread;
    _Atomic bool running;
    AudioRender render;
    void *userdata;
} AudioOut;

/* opens the default device (float, stereo preferred); fills rate/channels */
int audio_out_open(AudioOut *a);
int audio_out_start(AudioOut *a, AudioRender render, void *userdata);
void audio_out_stop(AudioOut *a);

#endif
