#include "app.h"
#include "../ftz.h"

#include <math.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

/* ---------- atomics ---------- */

void midi_note_press(MidiNoteAtom *m, uint8_t note) {
    atomic_store_explicit(&m->note, (int32_t)note, memory_order_relaxed);
}

void midi_note_release(MidiNoteAtom *m, uint8_t note) {
    int32_t expect = (int32_t)note;
    atomic_compare_exchange_strong_explicit(&m->note, &expect, -1,
                                            memory_order_relaxed,
                                            memory_order_relaxed);
}

void midi_note_clear(MidiNoteAtom *m) {
    atomic_store_explicit(&m->note, -1, memory_order_relaxed);
}

int midi_note_get(const MidiNoteAtom *m) {
    return atomic_load_explicit(&((MidiNoteAtom *)m)->note,
                                memory_order_relaxed);
}

void pitch_store(PitchAtom *p, float hz) {
    float q = fmaxf(hz, 0.0f) * 65536.0f;
    if (q > 4294967040.0f) q = 4294967040.0f;
    atomic_store_explicit(&p->q16, (uint32_t)q, memory_order_relaxed);
}

void voices_store(App *a, const VoiceBank *b) {
    pitch_store(&a->pitch, voice_bank_target_hz(b));
    float hz[POLY_MAX];
    int n = voice_bank_held_hz(b, hz);
    uint32_t mask = 0;
    for (int i = 0; i < n; i++) {
        if (!(hz[i] > 0.0f)) continue;
        int note = (int)roundf(69.0f + 12.0f * log2f(hz[i] / 440.0f));
        mask |= 1u << (((note % 12) + 12) % 12);
    }
    atomic_store_explicit(&a->held_pcs, mask, memory_order_relaxed);
    const Envelope *e = voice_bank_newest_env(b);
    uint32_t ms = (uint32_t)clampf(e->t * 1000.0f, 0.0f, (float)((1u << 30) - 1));
    atomic_store_explicit(&a->env_clock, (uint32_t)e->stage << 30 | ms,
                          memory_order_relaxed);
    atomic_store_explicit(&a->env_level_q16,
                          (uint32_t)(clampf(e->level, 0.0f, 1.0f) * 65536.0f),
                          memory_order_relaxed);
}

float pitch_load(const PitchAtom *p) {
    return (float)atomic_load_explicit(&((PitchAtom *)p)->q16,
                                       memory_order_relaxed)
           / 65536.0f;
}

void cc_write(CcState *c, uint8_t cc, uint8_t value) {
    _Atomic uint32_t *slot = &c->word[cc & 0x7f];
    uint32_t seq = atomic_load_explicit(slot, memory_order_relaxed) >> 8;
    atomic_store_explicit(slot, ((seq + 1) << 8) | value, memory_order_relaxed);
}

uint32_t cc_read(const CcState *c, uint8_t cc) {
    return atomic_load_explicit(&((CcState *)c)->word[cc & 0x7f],
                                memory_order_relaxed);
}

float cc_position(uint32_t word) {
    uint32_t v = word & 0xff;
    if (v > 127) v = 127;
    return (float)v / 127.0f;
}

const char *cc_target_name(CcTarget t) {
    switch (t) {
    case CC_INDEX: return "index";
    case CC_RIP: return "rip";
    case CC_FB: return "fb";
    case CC_RELEASE: return "release";
    case CC_GLIDE: return "glide";
    case CC_DRONEHZ: return "dronehz";
    case CC_MIX: return "mix";
    case CC_GHOST: return "ghost";
    case CC_DECAY: return "decay";
    case CC_DAMP: return "damp";
    case CC_HAUNT: return "haunt";
    case CC_ATTACK: return "attack";
    case CC_ENVDECAY: return "envdecay";
    case CC_SUSTAIN: return "sustain";
    default: return "?";
    }
}

CcTarget cc_target_from_name(const char *s) {
    for (int t = CC_INDEX; t <= CC_LAST; t++)
        if (strcmp(cc_target_name((CcTarget)t), s) == 0) return (CcTarget)t;
    return CC_NONE;
}

/* ---------- audio thread state ---------- */

typedef struct {
    App *app;
    VoiceBank voice;
    StereoVerb verb;
    Melody melody;
    PitchSeq pitch;
    Chandas chandas;
    Tape tape;
    Limiter limiter;
    Mod mod;
    ModBase base; /* what the controls say, before modulation */
    uint32_t decim;
    float peak_acc[2];
    bool midi_driving;
    bool rec_on;
    bool transport_running;
    float load_avg, load_peak;
} AudioState;

static AudioState g_as;

static float pole_k(float step_seconds, float tau_seconds) {
    return expf(-step_seconds / tau_seconds);
}

#define LOAD_AVG_SECONDS 0.4f
#define LOAD_PEAK_SECONDS 2.0f

/* writes base plus modulation into the engine for the groups that need it */
static void mod_tick(AudioState *s, size_t samples) {
    if (!mod_any_seq(&s->mod) && s->mod.groups_prev == 0) return;
    mod_advance(&s->mod, samples, chandas_tempo(&s->chandas));
    ModBase out;
    int g = mod_apply(&s->mod, &s->base, &out);
    if (g & MOD_G_PATCH) voice_bank_set_patch(&s->voice, out.patch);
    if (g & MOD_G_VERB) verb_set_params(&s->verb, out.verb);
    if (g & MOD_G_CHANDAS) chandas_set_params(&s->chandas, out.chandas);
    if (g & MOD_G_MELODY) melody_set_params(&s->melody, out.melody);
    if (g & MOD_G_WARMTH) tape_set(&s->tape, out.warmth);
    if (g & MOD_G_BEND) voice_bank_set_bend_semitones(&s->voice, out.bend);
}

static void engine_apply(AudioState *s, Event ev) {
    switch (ev.kind) {
    case EV_SET_PATCH:
        s->base.patch = ev.u.patch;
        voice_bank_set_patch(&s->voice, ev.u.patch);
        verb_configure(&s->verb, voice_bank_patch(&s->voice),
                       voice_bank_compiled(&s->voice));
        break;
    case EV_SET_VERB:
        s->base.verb = ev.u.verb;
        verb_set_params(&s->verb, ev.u.verb);
        break;
    case EV_SET_MELODY:
        s->base.melody = ev.u.melody;
        if (s->melody.params.enabled && !ev.u.melody.enabled)
            voice_bank_note_off_all(&s->voice);
        melody_set_params(&s->melody, ev.u.melody);
        break;
    case EV_SET_CHANDAS:
        s->base.chandas = ev.u.chandas;
        chandas_set_params(&s->chandas, ev.u.chandas);
        break;
    case EV_RESET_CHANDAS: chandas_reset(&s->chandas); break;
    case EV_SET_WARMTH:
        s->base.warmth = ev.u.f;
        tape_set(&s->tape, ev.u.f);
        break;
    case EV_SET_LIMITER:
        limiter_set(&s->limiter, ev.u.limiter.enabled, ev.u.limiter.ceiling_db);
        limiter_set_gain(&s->limiter, ev.u.limiter.gain_db);
        break;
    case EV_SET_SEQ: mod_set_seq(&s->mod, ev.u.seq.slot, ev.u.seq.p); break;
    case EV_SET_ROUTE:
        mod_set_route(&s->mod, ev.u.route.slot, ev.u.route.r);
        break;
    case EV_SET_TEMPO:
        chandas_set_tempo(&s->chandas, ev.u.f);
        melody_set_tempo(&s->melody, ev.u.f);
        pitch_seq_set_tempo(&s->pitch, ev.u.f);
        break;
    case EV_SET_PITCH: pitch_seq_set_params(&s->pitch, ev.u.pitch); break;
    case EV_SET_TRANSPORT:
        s->transport_running = ev.u.flag;
        chandas_set_transport(&s->chandas, ev.u.flag);
        break;
    case EV_PANIC: voice_bank_note_off_all(&s->voice); break;
    case EV_GLIDE_TO:
        voice_bank_drone_to_hz(&s->voice, ev.u.f);
        voice_bank_set_drone_hz(&s->voice, ev.u.f);
        verb_set_drone_hz(&s->verb, ev.u.f);
        break;
    case EV_NOTE_OFF: voice_bank_note_off(&s->voice, ev.u.note.key); break;
    case EV_BEND:
        s->base.bend = ev.u.f;
        voice_bank_set_bend_semitones(&s->voice, ev.u.f);
        break;
    case EV_NOTE_ON:
        voice_bank_note_on(&s->voice, ev.u.note.key, ev.u.note.hz,
                           ev.u.note.velocity);
        chandas_note_pulse(&s->chandas);
        mod_note_on(&s->mod);
        break;
    case EV_SET_ADSR: voice_bank_set_adsr_now(&s->voice, ev.u.adsr); break;
    case EV_ENGAGE: voice_bank_set_drone(&s->voice, ev.u.flag); break;
    case EV_SET_MIDI_DRIVING:
        /* a closed port never sends the NOTE_OFFs for keys still down */
        if (!ev.u.flag)
            for (int k = 0; k < 128; k++) voice_bank_note_off(&s->voice, k);
        break;
    case EV_RECORD:
        break;
    }
}

/* plays whatever the pitch sequencer has due now. A stopped transport holds
   the steps, but a note already sounding is still let go on time. While a
   MIDI port is driving, a gate-off releases only the sequencer's note. */
static void pitch_run_due(AudioState *s) {
    size_t until;
    while (pitch_seq_samples_until(&s->pitch, &until) && until == 0) {
        if (!s->transport_running && !s->pitch.held) break;
        PitchEvent e = pitch_seq_fire(&s->pitch);
        if (e.kind == PITCH_EV_NONE) break;
        if (e.kind == PITCH_EV_OFF || !s->midi_driving)
            pitch_event_play(e, &s->voice, &s->chandas, &s->mod);
    }
}

static void emit_frame(AudioState *s, float *data, size_t index, int channels,
                       bool rec_armed, const Frame *frame) {
    App *a = s->app;
    Stereo w = verb_process(&s->verb, frame);
    w = chandas_process(&s->chandas, w);
    Stereo limited = limiter_process(&s->limiter, w);
    float l = flush_tiny(limited.l), r = flush_tiny(limited.r);
    s->decim++;
    s->peak_acc[0] = fmaxf(s->peak_acc[0], fabsf(l));
    s->peak_acc[1] = fmaxf(s->peak_acc[1], fabsf(r));
    if (s->decim >= VIZ_DECIMATE) {
        s->decim = 0;
        VizFrame vf;
        memcpy(vf.ops, frame->ops, sizeof vf.ops);
        vf.l = l;
        vf.r = r;
        vf.peak[0] = s->peak_acc[0];
        vf.peak[1] = s->peak_acc[1];
        vf.limiter_reduction_db = limiter_reduction_db(&s->limiter);
        VizRing_push(&a->viz, vf);
        s->peak_acc[0] = s->peak_acc[1] = 0.0f;
    }
    size_t at = index * (size_t)channels;
    if (channels == 1) {
        float m = 0.5f * (l + r);
        data[at] = m;
        if (rec_armed) {
            RecRing_push(&a->rec, m);
            RecRing_push(&a->rec, m);
        }
    } else {
        float cl = l, cr = r;
        data[at] = cl;
        data[at + 1] = cr;
        for (int c = 2; c < channels; c++) data[at + c] = 0.0f;
        if (rec_armed) {
            RecRing_push(&a->rec, cl);
            RecRing_push(&a->rec, cr);
        }
    }
}

static void apply_ctrl(void *ud, Event ev);

static void render(void *ud, float *data, size_t frames, int channels) {
    AudioState *s = ud;
    App *a = s->app;
    struct timespec began;
    clock_gettime(CLOCK_MONOTONIC, &began);
    ftz_state csr = ftz_begin();

    app_drain_ctrl(a, apply_ctrl, s);
    Event ev;
    while (EventRing_pop(&a->midi_ev, &ev)) engine_apply(s, ev);

    bool rec_armed = s->rec_on;
    size_t done = 0;
    while (done < frames) {
        size_t until;
        pitch_run_due(s);
        if (s->transport_running && !s->pitch.params.enabled
            && melody_samples_until_fire(&s->melody, &until) && until == 0) {
            float hz = melody_fire(&s->melody);
            if (!s->midi_driving) {
                voice_bank_note_off_all(&s->voice);
                voice_bank_note_on(&s->voice, -1, hz, 1.0f);
                chandas_note_pulse(&s->chandas);
                mod_note_on(&s->mod);
            }
        }
        size_t run = frames - done;
        if (s->transport_running
            && melody_samples_until_fire(&s->melody, &until) && until < run)
            run = until;
        if ((s->transport_running || s->pitch.held)
            && pitch_seq_samples_until(&s->pitch, &until) && until < run)
            run = until;
        bool modulating = mod_any_seq(&s->mod) || s->mod.groups_prev;
        if (modulating && run > MOD_BLOCK) run = MOD_BLOCK;
        if (run < 1) run = 1;
        if (modulating) mod_tick(s, run);
        Frame chunk[BANK_CHUNK];
        size_t left = run;
        size_t at = done;
        while (left) {
            size_t n = left < BANK_CHUNK ? left : BANK_CHUNK;
            voice_bank_render_block(&s->voice, chunk, n);
            for (size_t i = 0; i < n; i++)
                emit_frame(s, data, at + i, channels, rec_armed, &chunk[i]);
            left -= n;
            at += n;
        }
        if (s->transport_running) melody_advance(&s->melody, run);
        if (s->transport_running || s->pitch.held)
            pitch_seq_advance(&s->pitch, run);
        done += run;
    }
    seq_meter_store(&a->seq_meter, &s->mod);
    atomic_store_explicit(&a->seq_meter.pitch_step, s->pitch.step,
                          memory_order_relaxed);

    float budget = (float)frames / a->sample_rate;
    if (budget > 0.0f) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        float elapsed = (float)(now.tv_sec - began.tv_sec)
                        + (float)(now.tv_nsec - began.tv_nsec) * 1e-9f;
        float load = elapsed / budget;
        float k_avg = pole_k(budget, LOAD_AVG_SECONDS);
        float k_peak = pole_k(budget, LOAD_PEAK_SECONDS);
        s->load_avg = load + (s->load_avg - load) * k_avg;
        s->load_peak = fmaxf(s->load_peak * k_peak, load);
        uint32_t ql = (uint32_t)(clampf(s->load_avg, 0.0f, 65535.0f) * 65536.0f);
        uint32_t qp = (uint32_t)(clampf(s->load_peak, 0.0f, 65535.0f) * 65536.0f);
        atomic_store_explicit(&a->meter.load_q16, ql, memory_order_relaxed);
        atomic_store_explicit(&a->meter.peak_q16, qp, memory_order_relaxed);
        atomic_store_explicit(&a->meter.frames, (uint32_t)frames,
                              memory_order_relaxed);
    }
    voices_store(a, &s->voice);
    ftz_end(csr);
}

int gui_audio_start(App *a) {
    if (audio_out_open(&a->audio) != 0) return -1;
    a->sample_rate = (float)a->audio.rate;
    a->channels = a->audio.channels;

    AudioState *s = &g_as;
    memset(s, 0, sizeof *s);
    s->app = a;
    voice_bank_init(&s->voice, a->sample_rate, a->shadow);
    voice_bank_set_freq_hz(&s->voice, START_HZ);
    verb_init(&s->verb, a->sample_rate);
    verb_configure(&s->verb, voice_bank_patch(&s->voice),
                   voice_bank_compiled(&s->voice));
    verb_set_params(&s->verb, a->shadow_verb);
    melody_init(&s->melody, a->sample_rate, melody_params_default());
    pitch_seq_init(&s->pitch, a->sample_rate);
    chandas_init(&s->chandas, a->sample_rate);
    tape_init(&s->tape, a->sample_rate);
    limiter_init(&s->limiter, a->sample_rate);
    limiter_set(&s->limiter, a->shadow_limiter_enabled, a->shadow_limiter_ceiling_db);
    limiter_set_gain(&s->limiter, a->shadow_output_gain_db);
    mod_init(&s->mod, a->sample_rate);
    s->base.patch = a->shadow;
    s->base.verb = a->shadow_verb;
    s->base.chandas = chandas_params(&s->chandas);
    s->base.melody = melody_params_default();
    s->base.warmth = tape_warmth(&s->tape);
    s->base.bend = 0.0f;
    s->transport_running = true;
    voice_bank_set_drone(&s->voice, a->engaged);
    midi_note_clear(&a->midi_note);
    return audio_out_start(&a->audio, render, s);
}

void gui_audio_stop(App *a) {
    audio_out_stop(&a->audio);
    chandas_free(&g_as.chandas);
    limiter_free(&g_as.limiter);
    verb_free(&g_as.verb);
    voice_bank_free(&g_as.voice);
}

/* ---------- MIDI ---------- */

bool midi_driving(const App *a) { return a->midi_open; }

int midi_port_names(char names[][128], int max) {
    return midi_list_sources(names, max);
}

static void on_midi(void *ud, const uint8_t msg[3]) {
    App *a = ud;
    uint8_t status = msg[0] & 0xF0;
    Event ev;
    switch (status) {
    case 0x90:
        if (msg[2] > 0) {
            ev.kind = EV_NOTE_ON;
            ev.u.note.key = msg[1];
            ev.u.note.hz = midi_to_hz(msg[1]);
            ev.u.note.velocity = (float)(msg[2] > 127 ? 127 : msg[2]) / 127.0f;
            EventRing_push(&a->midi_ev, ev);
            midi_note_press(&a->midi_note, msg[1]);
        } else {
            ev.kind = EV_NOTE_OFF;
            ev.u.note.key = msg[1];
            EventRing_push(&a->midi_ev, ev);
            midi_note_release(&a->midi_note, msg[1]);
        }
        break;
    case 0x80:
        ev.kind = EV_NOTE_OFF;
        ev.u.note.key = msg[1];
        EventRing_push(&a->midi_ev, ev);
        midi_note_release(&a->midi_note, msg[1]);
        break;
    case 0xE0: {
        int raw = ((msg[2] & 0x7f) << 7) | (msg[1] & 0x7f);
        ev.kind = EV_BEND;
        ev.u.f = (float)(raw - 8192) / 8192.0f * BEND_SEMITONES;
        EventRing_push(&a->midi_ev, ev);
        break;
    }
    case 0xB0:
        cc_write(&a->cc, msg[1], msg[2]);
        break;
    default:
        break;
    }
}

bool gui_set_midi_port(App *a, const char *name) {
    if (a->midi_open) {
        midi_in_stop(&a->midi);
        a->midi_open = false;
        a->midi_port[0] = '\0';
        midi_note_clear(&a->midi_note);
        for (int i = 0; i < 128; i++) {
            atomic_store_explicit(&a->cc.word[i], 0, memory_order_relaxed);
            a->cc_seen[i] = 0;
            a->cc_heard[i] = false;
        }
        Event ev = {.kind = EV_BEND, .u.f = 0.0f};
        app_send(a, ev);
        ev.kind = EV_SET_MIDI_DRIVING;
        ev.u.flag = false;
        app_send(a, ev);
    }
    if (!name) return true;
    if (midi_in_start_named(&a->midi, on_midi, a, name) != 0) return false;
    a->midi_open = true;
    snprintf(a->midi_port, sizeof a->midi_port, "%s", name);
    Event ev = {.kind = EV_SET_MIDI_DRIVING, .u.flag = true};
    app_send(a, ev);
    return true;
}

/* ---------- UI-side drains ---------- */

static void apply_ctrl(void *ud, Event ev) {
    AudioState *s = ud;
    if (ev.kind == EV_RECORD) s->rec_on = ev.u.flag;
    if (ev.kind == EV_SET_MIDI_DRIVING) s->midi_driving = ev.u.flag;
    engine_apply(s, ev);
}

void app_send_mods(App *a) {
    for (int i = 0; i < SEQS; i++)
        app_send(a, (Event){.kind = EV_SET_SEQ, .u.seq = {i, a->mods.seq[i]}});
    for (int i = 0; i < MOD_ROUTES; i++)
        app_send(a, (Event){.kind = EV_SET_ROUTE,
                            .u.route = {i, a->mods.route[i]}});
}

void gui_drain_viz(App *a) {
    VizFrame f;
    while (VizRing_pop(&a->viz, &f)) {
        for (int i = 0; i < NUM_OPS; i++) {
            float x = fabsf(f.ops[i]);
            a->env[i] = x > a->env[i] ? x : a->env[i] * 0.9985f;
        }
        a->lissa_x[a->lissa_head] = f.l;
        a->lissa_y[a->lissa_head] = f.r;
        a->limiter_reduction_db = f.limiter_reduction_db;
        a->lissa_head = (a->lissa_head + 1) % 512;
        if (a->lissa_len < 512) a->lissa_len++;
    }
}

/* a CC moves its control the way the fader does, along the same curve */
static const ParamId CC_PARAM[CC_LAST + 1] = {
    [CC_INDEX] = PARAM_INDEX,       [CC_RIP] = PARAM_RIP,
    [CC_FB] = PARAM_FB,             [CC_RELEASE] = PARAM_RELEASE,
    [CC_GLIDE] = PARAM_GLIDE,       [CC_DRONEHZ] = PARAM_DRONE_HZ,
    [CC_MIX] = PARAM_MIX,           [CC_GHOST] = PARAM_GHOST,
    [CC_DECAY] = PARAM_VERB_DECAY,  [CC_DAMP] = PARAM_DAMP,
    [CC_HAUNT] = PARAM_HAUNT,
    [CC_ATTACK] = PARAM_ATTACK,     [CC_ENVDECAY] = PARAM_ENV_DECAY,
    [CC_SUSTAIN] = PARAM_SUSTAIN,
};

void gui_apply_cc(App *a, Ui *ui) {
    (void)ui;
    int groups = 0;
    for (int cc = 0; cc < 128; cc++) {
        uint32_t word = cc_read(&a->cc, (uint8_t)cc);
        if (word == a->cc_seen[cc]) continue;
        a->cc_seen[cc] = word;
        if (word == 0) continue;
        if (!a->cc_heard[cc]) {
            a->cc_heard[cc] = true;
            uint32_t v = word & 0xff;
            if (a->cc_bind[cc] != CC_NONE)
                push_log(a, "cc%d is here, at %u, driving %s.", cc, v,
                         cc_target_name(a->cc_bind[cc]));
            else
                push_log(a,
                         "cc%d is here, at %u, bound to nothing. bind cc%d - "
                         "<control>.",
                         cc, v, cc);
        }
        CcTarget target = a->cc_bind[cc];
        if (target == CC_NONE) continue;
        ParamId id = CC_PARAM[target];
        param_set(a, id, param_at(id, cc_position(word)));
        groups |= PARAMS[id].group;
    }
    params_send(a, groups);
}

void gui_run_bind(App *a, int cc, CcTarget target) {
    char line[LOG_LINE_LEN];
    int n = snprintf(line, sizeof line, "cc%d drives %s.", cc,
                     cc_target_name(target));
    if (a->cc_bind[cc] != CC_NONE && a->cc_bind[cc] != target)
        n += snprintf(line + n, sizeof line - (size_t)n, " cc%d let go of %s.",
                      cc, cc_target_name(a->cc_bind[cc]));
    for (int other = 0; other < 128; other++) {
        if (other != cc && a->cc_bind[other] == target) {
            a->cc_bind[other] = CC_NONE;
            n += snprintf(line + n, sizeof line - (size_t)n,
                          " %s let go of cc%d.", cc_target_name(target), other);
        }
    }
    a->cc_bind[cc] = target;
    a->cc_seen[cc] = cc_read(&a->cc, (uint8_t)cc);
    if (!midi_driving(a))
        snprintf(line + n, sizeof line - (size_t)n,
                 " no midi port is open, so nothing is sending it yet.");
    push_log(a, "%s", line);
}

void gui_run_unbind(App *a, int cc) {
    if (cc >= 0) {
        if (a->cc_bind[cc] != CC_NONE) {
            push_log(a, "cc%d no longer drives %s.", cc,
                     cc_target_name(a->cc_bind[cc]));
            a->cc_bind[cc] = CC_NONE;
        } else {
            push_log(a, "cc%d was not bound to anything.", cc);
        }
        return;
    }
    int n = 0;
    for (int i = 0; i < 128; i++) {
        if (a->cc_bind[i] != CC_NONE) n++;
        a->cc_bind[i] = CC_NONE;
    }
    if (n == 0)
        push_log(a, "nothing was bound.");
    else if (n == 1)
        push_log(a, "one binding let go.");
    else
        push_log(a, "%d bindings let go.", n);
}

/* ---------- log ---------- */

void push_log(App *a, const char *fmt, ...) {
    char line[LOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    a->log_head = (a->log_head + 1) % LOG_LINES;
    snprintf(a->log[a->log_head], LOG_LINE_LEN, "%s", line);
    a->log_place[a->log_head] = GRAPH_NONE;
    if (a->log_len < LOG_LINES) a->log_len++;
    /* restart the typewriter on the newest line */
    snprintf(a->console_typing, sizeof a->console_typing, "%s", line);
    a->console_revealed = 0;
    a->console_credit = 0.0f;
}

void push_log_view(App *a, const View *v) {
    for (int i = 0; i < v->n; i++) {
        push_log(a, "%s", v->line[i].text);
        a->log_place[a->log_head] = v->line[i].place;
        a->log_graph[a->log_head] = v->line[i].graph;
    }
}

/* ---------- recording ---------- */

static void slug_name(const char *raw, char *out, size_t len) {
    size_t o = 0;
    bool dash = false;
    for (const char *p = raw; *p && o + 1 < len; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[o++] = c;
            dash = false;
        } else if (c >= 'A' && c <= 'Z') {
            out[o++] = (char)(c + 32);
            dash = false;
        } else if (!dash && o > 0) {
            out[o++] = '-';
            dash = true;
        }
    }
    while (o > 0 && out[o - 1] == '-') o--;
    out[o] = '\0';
    if (o == 0) snprintf(out, len, "take");
}

bool recorder_start(Recorder *r, const char *preset_name, float sample_rate) {
    slug_name(preset_name ? preset_name : "take", r->preset_slug,
              sizeof r->preset_slug);
    snprintf(r->path, sizeof r->path, "%s/bypo-%s-%ld.wav", recording_dir(),
             r->preset_slug, (long)time(NULL));
    r->file = fopen(r->path, "wb");
    if (!r->file) return false;
    r->frames = 0;
    r->sample_rate = sample_rate;
    /* 32-bit float stereo WAV header; sizes patched in finalize */
    uint32_t rate = (uint32_t)sample_rate;
    uint32_t byte_rate = rate * 2 * 4;
    uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
                       'f', 'm', 't', ' ', 16, 0, 0, 0, 3, 0, 2, 0};
    memcpy(hdr + 24, &rate, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    hdr[32] = 8; /* block align */
    hdr[34] = 32; /* bits */
    memcpy(hdr + 36, "data", 4);
    fwrite(hdr, 1, 44, r->file);
    return true;
}

void recorder_push(Recorder *r, const float *interleaved, size_t samples) {
    if (!r->file) return;
    fwrite(interleaved, sizeof(float), samples, r->file);
    r->frames += samples / 2;
}

void recorder_finalize(Recorder *r) {
    if (!r->file) return;
    uint32_t data = (uint32_t)(r->frames * 2 * 4);
    uint32_t riff = 36 + data;
    fseek(r->file, 4, SEEK_SET);
    fwrite(&riff, 4, 1, r->file);
    fseek(r->file, 40, SEEK_SET);
    fwrite(&data, 4, 1, r->file);
    fclose(r->file);
    r->file = NULL;
}

void gui_run_record(App *a, const char *args) {
    /* args: optional name, optional trailing "--end <secs>" */
    char name[128] = "";
    float end = -1.0f;
    if (args && *args) {
        const char *e = strstr(args, "--end");
        if (e) {
            end = strtof(e + 5, NULL);
            size_t n = (size_t)(e - args);
            while (n > 0 && args[n - 1] == ' ') n--;
            if (n >= sizeof name) n = sizeof name - 1;
            memcpy(name, args, n);
            name[n] = '\0';
        } else {
            snprintf(name, sizeof name, "%s", args);
        }
    }
    if (a->recording) {
        if (end >= 0.0f) {
            push_log(a, "a take is already running. rec on its own finishes it.");
            return;
        }
        gui_stop_record(a);
        return;
    }
    char label[192];
    if (name[0])
        snprintf(label, sizeof label, "%s", name);
    else if (a->have_loaded)
        preset_qualified(&a->preset_loaded, label, sizeof label);
    else
        label[0] = '\0';
    if (!recorder_start(&a->recorder, label[0] ? label : NULL, a->sample_rate)) {
        push_log(a, "record failed: could not open the file.");
        return;
    }
    a->recording = true;
    a->rec_stop_pending = false;
    a->have_rec_stop_at = end >= 0.0f;
    if (a->have_rec_stop_at) {
        a->rec_stop_at = a->last_frame_time + (double)end;
        push_log(a, "recording %.0f s → %s", (double)end, a->recorder.path);
    } else {
        push_log(a, "recording → %s", a->recorder.path);
    }
    Event ev = {.kind = EV_RECORD, .u.flag = true};
    app_send(a, ev);
}

void gui_stop_record(App *a) {
    Event ev = {.kind = EV_RECORD, .u.flag = false};
    app_send(a, ev);
    a->rec_stop_pending = true;
    a->rec_quiet_frames = 0;
    a->have_rec_stop_at = false;
}

void gui_drain_recording(App *a) {
    if (a->have_rec_stop_at && a->last_frame_time >= a->rec_stop_at)
        gui_stop_record(a);
    bool drained_any = false;
    float x;
    float buf[1024];
    size_t n = 0;
    while (RecRing_pop(&a->rec, &x)) {
        drained_any = true;
        if (a->recording) {
            buf[n++] = x;
            if (n == sizeof buf / sizeof buf[0]) {
                recorder_push(&a->recorder, buf, n);
                n = 0;
            }
        }
    }
    if (n > 0 && a->recording) recorder_push(&a->recorder, buf, n);
    if (!a->rec_stop_pending) return;
    if (drained_any)
        a->rec_quiet_frames = 0;
    else
        a->rec_quiet_frames++;
    if (a->rec_quiet_frames < 3) return;
    if (a->recording) {
        double secs = (double)a->recorder.frames / (double)a->sample_rate;
        char path[512];
        snprintf(path, sizeof path, "%s", a->recorder.path);
        recorder_finalize(&a->recorder);
        a->recording = false;
        push_log(a, "recorded %.1f s → %s", secs, path);
    }
    a->rec_stop_pending = false;
}
