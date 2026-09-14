#ifndef BYPO_MIDI_H
#define BYPO_MIDI_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#if defined(__linux__)
#include <alsa/asoundlib.h>
#endif

/* raw channel-voice message, 3 bytes (status, data1, data2) */
typedef void (*MidiMessage)(void *userdata, const uint8_t msg[3]);

typedef struct {
#if defined(__linux__)
    snd_seq_t *seq;
    int port;
    pthread_t thread;
#elif defined(__APPLE__)
    uint32_t client; /* MIDIClientRef; 0 when closed */
    uint32_t port;   /* MIDIPortRef */
    uint32_t source; /* MIDIEndpointRef we are connected to */
#elif defined(_WIN32)
    void *hmidi; /* HMIDIIN; NULL when closed */
#endif
    _Atomic bool running;
    MidiMessage on_message;
    void *userdata;
    char source_name[128];
} MidiIn;

/* connects to the first real readable MIDI source; -1 if none */
int midi_in_start(MidiIn *m, MidiMessage on_message, void *userdata);
void midi_in_stop(MidiIn *m);

/* list readable sources (one display name per row); returns count */
int midi_list_sources(char names[][128], int max);
/* connect to the source whose display name matches `name` exactly */
int midi_in_start_named(MidiIn *m, MidiMessage on_message, void *userdata,
                        const char *name);

#endif
