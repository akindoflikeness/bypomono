#include "midi.h"

#include <stdio.h>
#include <string.h>

#if defined(__linux__)

/* ---------- ALSA sequencer: the original backend ---------- */

#include <alloca.h>

static bool find_source(snd_seq_t *seq, int *client, int *port, char *name,
                        size_t name_len) {
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int c = snd_seq_client_info_get_client(cinfo);
        if (c == SND_SEQ_CLIENT_SYSTEM) continue;
        const char *cname = snd_seq_client_info_get_name(cinfo);
        if (strstr(cname, "Midi Through")) continue;
        snd_seq_port_info_set_client(pinfo, c);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
                != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
                continue;
            *client = c;
            *port = snd_seq_port_info_get_port(pinfo);
            snprintf(name, name_len, "%s", cname);
            return true;
        }
    }
    return false;
}

static void *midi_thread(void *arg) {
    MidiIn *m = arg;
    int npfd = snd_seq_poll_descriptors_count(m->seq, POLLIN);
    struct pollfd pfd[4];
    if (npfd > 4) npfd = 4;
    snd_seq_poll_descriptors(m->seq, pfd, npfd, POLLIN);
    while (atomic_load(&m->running)) {
        if (poll(pfd, npfd, 100) <= 0) continue;
        snd_seq_event_t *ev;
        while (snd_seq_event_input(m->seq, &ev) >= 0) {
            uint8_t msg[3] = {0, 0, 0};
            switch (ev->type) {
            case SND_SEQ_EVENT_NOTEON:
                msg[0] = 0x90 | (ev->data.note.channel & 0x0F);
                msg[1] = ev->data.note.note;
                msg[2] = ev->data.note.velocity;
                break;
            case SND_SEQ_EVENT_NOTEOFF:
                msg[0] = 0x80 | (ev->data.note.channel & 0x0F);
                msg[1] = ev->data.note.note;
                msg[2] = ev->data.note.velocity;
                break;
            case SND_SEQ_EVENT_PITCHBEND: {
                int v = ev->data.control.value + 8192;
                msg[0] = 0xE0 | (ev->data.control.channel & 0x0F);
                msg[1] = v & 0x7F;
                msg[2] = (v >> 7) & 0x7F;
                break;
            }
            case SND_SEQ_EVENT_CONTROLLER:
                msg[0] = 0xB0 | (ev->data.control.channel & 0x0F);
                msg[1] = ev->data.control.param & 0x7F;
                msg[2] = ev->data.control.value & 0x7F;
                break;
            default:
                continue;
            }
            m->on_message(m->userdata, msg);
        }
    }
    return NULL;
}

int midi_list_sources(char names[][128], int max) {
    snd_seq_t *seq;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0)
        return 0;
    int count = 0;
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0 && count < max) {
        int c = snd_seq_client_info_get_client(cinfo);
        if (c == SND_SEQ_CLIENT_SYSTEM) continue;
        snd_seq_port_info_set_client(pinfo, c);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0 && count < max) {
            unsigned caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
                != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
                continue;
            snprintf(names[count], 128, "%s: %s",
                     snd_seq_client_info_get_name(cinfo),
                     snd_seq_port_info_get_name(pinfo));
            count++;
        }
    }
    snd_seq_close(seq);
    return count;
}

static int start_connected(MidiIn *m, MidiMessage on_message, void *userdata,
                           int client, int port);

int midi_in_start_named(MidiIn *m, MidiMessage on_message, void *userdata,
                        const char *name) {
    m->seq = NULL;
    snd_seq_t *seq;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0)
        return -1;
    int client = -1, port = -1;
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0 && client < 0) {
        int c = snd_seq_client_info_get_client(cinfo);
        if (c == SND_SEQ_CLIENT_SYSTEM) continue;
        snd_seq_port_info_set_client(pinfo, c);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned caps = snd_seq_port_info_get_capability(pinfo);
            if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
                != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
                continue;
            char full[128];
            snprintf(full, sizeof full, "%s: %s",
                     snd_seq_client_info_get_name(cinfo),
                     snd_seq_port_info_get_name(pinfo));
            if (strcmp(full, name) == 0) {
                client = c;
                port = snd_seq_port_info_get_port(pinfo);
                snprintf(m->source_name, sizeof m->source_name, "%s", full);
                break;
            }
        }
    }
    snd_seq_close(seq);
    if (client < 0) return -1;
    return start_connected(m, on_message, userdata, client, port);
}

static int start_connected(MidiIn *m, MidiMessage on_message, void *userdata,
                           int client, int port) {
    m->seq = NULL;
    atomic_store(&m->running, false);
    if (snd_seq_open(&m->seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0)
        return -1;
    snd_seq_set_client_name(m->seq, "blow-your-phase-off");
    m->port = snd_seq_create_simple_port(
        m->seq, "bypo-in",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (m->port < 0 || snd_seq_connect_from(m->seq, m->port, client, port) < 0) {
        snd_seq_close(m->seq);
        m->seq = NULL;
        return -1;
    }
    m->on_message = on_message;
    m->userdata = userdata;
    atomic_store(&m->running, true);
    if (pthread_create(&m->thread, NULL, midi_thread, m) != 0) {
        atomic_store(&m->running, false);
        snd_seq_close(m->seq);
        m->seq = NULL;
        return -1;
    }
    return 0;
}

int midi_in_start(MidiIn *m, MidiMessage on_message, void *userdata) {
    snd_seq_t *seq;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0)
        return -1;
    int client, port;
    bool found = find_source(seq, &client, &port, m->source_name,
                             sizeof m->source_name);
    snd_seq_close(seq);
    if (!found) return -1;
    return start_connected(m, on_message, userdata, client, port);
}

void midi_in_stop(MidiIn *m) {
    if (!m->seq) return;
    if (atomic_exchange(&m->running, false)) pthread_join(m->thread, NULL);
    snd_seq_close(m->seq);
    m->seq = NULL;
}

#elif defined(__APPLE__)

/* ---------- CoreMIDI ----------
   CoreMIDI delivers packets on its own high-priority thread, so there is no
   thread of ours to run; `running` gates delivery the same way it gates the
   ALSA loop. A packet may carry several messages back to back, so the read
   proc walks the bytes and hands over each complete 3-byte channel-voice
   message on its own. */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

static void endpoint_name(MIDIEndpointRef ep, char *out, size_t len) {
    CFStringRef s = NULL;
    out[0] = 0;
    if (MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &s) != noErr
        || !s) {
        if (MIDIObjectGetStringProperty(ep, kMIDIPropertyName, &s) != noErr
            || !s) {
            snprintf(out, len, "MIDI source");
            return;
        }
    }
    if (!CFStringGetCString(s, out, (CFIndex)len, kCFStringEncodingUTF8))
        snprintf(out, len, "MIDI source");
    CFRelease(s);
}

static int status_len(uint8_t status) {
    switch (status & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 3;
    case 0xC0: case 0xD0: return 2;
    default: return 0; /* system messages: skipped */
    }
}

static void read_proc(const MIDIPacketList *list, void *ref, void *src) {
    (void)src;
    MidiIn *m = ref;
    if (!atomic_load(&m->running)) return;
    const MIDIPacket *p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; i++) {
        const Byte *b = p->data;
        UInt16 n = p->length;
        UInt16 k = 0;
        while (k < n) {
            uint8_t st = b[k];
            if (st >= 0xF8) { k++; continue; }          /* realtime */
            if (st == 0xF0) break;                      /* sysex: drop packet */
            if (st < 0x80) { k++; continue; }           /* stray data byte */
            int len = status_len(st);
            if (len == 0 || k + len > n) break;
            if (len == 3) {
                uint8_t msg[3] = {st, (uint8_t)(b[k + 1] & 0x7F),
                                  (uint8_t)(b[k + 2] & 0x7F)};
                switch (st & 0xF0) {
                case 0x80: case 0x90: case 0xB0: case 0xE0:
                    m->on_message(m->userdata, msg);
                    break;
                default: break;
                }
            }
            k += (UInt16)len;
        }
        p = MIDIPacketNext(p);
    }
}

int midi_list_sources(char names[][128], int max) {
    ItemCount n = MIDIGetNumberOfSources();
    int count = 0;
    for (ItemCount i = 0; i < n && count < max; i++) {
        MIDIEndpointRef ep = MIDIGetSource(i);
        if (!ep) continue;
        endpoint_name(ep, names[count], 128);
        count++;
    }
    return count;
}

static int start_connected(MidiIn *m, MidiMessage on_message, void *userdata,
                           MIDIEndpointRef ep) {
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    if (MIDIClientCreate(CFSTR("blow-your-phase-off"), NULL, NULL, &client)
        != noErr)
        return -1;
    if (MIDIInputPortCreate(client, CFSTR("bypo-in"), read_proc, m, &port)
        != noErr) {
        MIDIClientDispose(client);
        return -1;
    }
    m->on_message = on_message;
    m->userdata = userdata;
    atomic_store(&m->running, true);
    if (MIDIPortConnectSource(port, ep, NULL) != noErr) {
        atomic_store(&m->running, false);
        MIDIPortDispose(port);
        MIDIClientDispose(client);
        return -1;
    }
    m->client = (uint32_t)client;
    m->port = (uint32_t)port;
    m->source = (uint32_t)ep;
    return 0;
}

int midi_in_start_named(MidiIn *m, MidiMessage on_message, void *userdata,
                        const char *name) {
    m->client = 0;
    ItemCount n = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < n; i++) {
        MIDIEndpointRef ep = MIDIGetSource(i);
        if (!ep) continue;
        char full[128];
        endpoint_name(ep, full, sizeof full);
        if (strcmp(full, name) == 0) {
            snprintf(m->source_name, sizeof m->source_name, "%s", full);
            return start_connected(m, on_message, userdata, ep);
        }
    }
    return -1;
}

int midi_in_start(MidiIn *m, MidiMessage on_message, void *userdata) {
    m->client = 0;
    if (MIDIGetNumberOfSources() == 0) return -1;
    MIDIEndpointRef ep = MIDIGetSource(0);
    if (!ep) return -1;
    endpoint_name(ep, m->source_name, sizeof m->source_name);
    return start_connected(m, on_message, userdata, ep);
}

void midi_in_stop(MidiIn *m) {
    if (!m->client) return;
    atomic_store(&m->running, false);
    MIDIPortDisconnectSource((MIDIPortRef)m->port, (MIDIEndpointRef)m->source);
    MIDIPortDispose((MIDIPortRef)m->port);
    MIDIClientDispose((MIDIClientRef)m->client);
    m->client = 0;
    m->port = 0;
    m->source = 0;
}

#elif defined(_WIN32)

/* ---------- WinMM ----------
   The classic multimedia MIDI API: every Windows since 3.1 has it, it needs
   no COM setup, and it delivers each short message as one packed DWORD on
   its own callback thread, which is exactly the 3-byte shape the engine
   wants. */

#include <windows.h>
#include <mmsystem.h>

static void CALLBACK midi_proc(HMIDIIN h, UINT msg, DWORD_PTR inst,
                               DWORD_PTR p1, DWORD_PTR p2) {
    (void)h; (void)p2;
    if (msg != MIM_DATA) return;
    MidiIn *m = (MidiIn *)inst;
    if (!m || !atomic_load(&m->running)) return;
    uint8_t st = (uint8_t)(p1 & 0xFF);
    switch (st & 0xF0) {
    case 0x80: case 0x90: case 0xB0: case 0xE0: {
        uint8_t out[3] = {st, (uint8_t)((p1 >> 8) & 0x7F),
                          (uint8_t)((p1 >> 16) & 0x7F)};
        m->on_message(m->userdata, out);
        break;
    }
    default: break;
    }
}

static void device_name(UINT id, char *out, size_t len) {
    MIDIINCAPSA caps;
    if (midiInGetDevCapsA(id, &caps, sizeof caps) == MMSYSERR_NOERROR)
        snprintf(out, len, "%s", caps.szPname);
    else
        snprintf(out, len, "MIDI input %u", id);
}

int midi_list_sources(char names[][128], int max) {
    UINT n = midiInGetNumDevs();
    int count = 0;
    for (UINT i = 0; i < n && count < max; i++) {
        device_name(i, names[count], 128);
        count++;
    }
    return count;
}

static int start_connected(MidiIn *m, MidiMessage on_message, void *userdata,
                           UINT id) {
    HMIDIIN h = NULL;
    m->on_message = on_message;
    m->userdata = userdata;
    atomic_store(&m->running, true);
    if (midiInOpen(&h, id, (DWORD_PTR)midi_proc, (DWORD_PTR)m,
                   CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        atomic_store(&m->running, false);
        return -1;
    }
    if (midiInStart(h) != MMSYSERR_NOERROR) {
        atomic_store(&m->running, false);
        midiInClose(h);
        return -1;
    }
    m->hmidi = h;
    return 0;
}

int midi_in_start_named(MidiIn *m, MidiMessage on_message, void *userdata,
                        const char *name) {
    m->hmidi = NULL;
    UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        char full[128];
        device_name(i, full, sizeof full);
        if (strcmp(full, name) == 0) {
            snprintf(m->source_name, sizeof m->source_name, "%s", full);
            return start_connected(m, on_message, userdata, i);
        }
    }
    return -1;
}

int midi_in_start(MidiIn *m, MidiMessage on_message, void *userdata) {
    m->hmidi = NULL;
    if (midiInGetNumDevs() == 0) return -1;
    device_name(0, m->source_name, sizeof m->source_name);
    return start_connected(m, on_message, userdata, 0);
}

void midi_in_stop(MidiIn *m) {
    if (!m->hmidi) return;
    atomic_store(&m->running, false);
    HMIDIIN h = (HMIDIIN)m->hmidi;
    midiInStop(h);
    midiInReset(h);
    midiInClose(h);
    m->hmidi = NULL;
}

#else
#error "no MIDI backend for this platform"
#endif
