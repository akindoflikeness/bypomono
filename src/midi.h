#ifndef BYPO_MIDI_H
#define BYPO_MIDI_H

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* raw channel-voice message, 3 bytes (status, data1, data2) */
typedef void (*MidiMessage)(void *userdata, const uint8_t msg[3]);

typedef struct {
    snd_seq_t *seq;
    int port;
    pthread_t thread;
    _Atomic bool running;
    MidiMessage on_message;
    void *userdata;
    char source_name[128];
} MidiIn;

/* connects to the first real readable MIDI source; -1 if none */
int midi_in_start(MidiIn *m, MidiMessage on_message, void *userdata);
void midi_in_stop(MidiIn *m);

/* list readable sources ("client:port name" per row); returns count */
int midi_list_sources(char names[][128], int max);
/* connect to the source whose display name matches `name` exactly */
int midi_in_start_named(MidiIn *m, MidiMessage on_message, void *userdata,
                        const char *name);

#endif
