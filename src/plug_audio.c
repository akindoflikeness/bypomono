/* The host hands a CLAP plugin its audio, so the plugin links these in place
   of src/audio.c: nothing in the plugin calls them, and src/audio.c would drag
   SDL2 into the macOS and Windows builds. */

#include "audio.h"

int audio_out_open(AudioOut *a) {
    (void)a;
    return -1;
}

int audio_out_start(AudioOut *a, AudioRender render, void *userdata) {
    (void)a;
    (void)render;
    (void)userdata;
    return -1;
}

void audio_out_stop(AudioOut *a) { (void)a; }
