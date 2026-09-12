#include "midi.h"

#include <alloca.h>
#include <stdio.h>
#include <string.h>

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
