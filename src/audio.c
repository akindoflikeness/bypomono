#include "audio.h"

#include <stdio.h>

#define CHUNK_FRAMES 256

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
