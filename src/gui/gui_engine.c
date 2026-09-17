#include "app.h"

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
    case CC_FIELD: return "field";
    case CC_CURVE: return "curve";
    case CC_RELEASE: return "release";
    case CC_GLIDE: return "glide";
    case CC_DRONEHZ: return "dronehz";
    case CC_MIX: return "mix";
    case CC_GHOST: return "ghost";
    case CC_DECAY: return "decay";
    case CC_DAMP: return "damp";
    case CC_HAUNT: return "haunt";
    case CC_WARMTH: return "warmth";
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
    Chandas chandas;
    Tape tape;
    EngageGate gate;
    Mod mod;
    ModBase base; /* what the controls say, before modulation */
    uint32_t decim;
    float peak_acc[2];
    bool engaged;
    bool midi_driving;
    bool rec_on;
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
    if (!mod_any_lfo(&s->mod) && s->mod.groups_prev == 0) return;
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
    case EV_SET_LFO: mod_set_lfo(&s->mod, ev.u.lfo.slot, ev.u.lfo.p); break;
    case EV_SET_ROUTE:
        mod_set_route(&s->mod, ev.u.route.slot, ev.u.route.r);
        break;
    case EV_SET_TEMPO: chandas_set_tempo(&s->chandas, ev.u.f); break;
    case EV_GLIDE_TO:
        voice_bank_glide_to_hz(&s->voice, ev.u.f);
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
    case EV_SET_CHAIN: {
        State next = voice_bank_state(&s->voice);
        next.chain = ev.u.chain;
        voice_bank_set_state(&s->voice, next);
        break;
    }
    case EV_ENGAGE:
    case EV_RECORD:
    case EV_SET_MIDI_DRIVING:
        break;
    }
}

typedef struct {
    AudioState *s;
    float *data;
    size_t base;
    int channels;
    bool rec_armed;
    bool notes_live;
} RenderCtx;

static void emit_frame(void *ud, size_t n, const Frame *frame) {
    RenderCtx *ctx = ud;
    AudioState *s = ctx->s;
    App *a = s->app;
    Stereo w = verb_process(&s->verb, frame);
    w = chandas_process(&s->chandas, w);
    w = tape_process(&s->tape, w);
    float g = engage_gate_next(&s->gate, s->engaged || ctx->notes_live);
    float l = w.l * g, r = w.r * g;
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
        VizRing_push(&a->viz, vf);
        s->peak_acc[0] = s->peak_acc[1] = 0.0f;
    }
    size_t at = (ctx->base + n) * (size_t)ctx->channels;
    if (ctx->channels == 1) {
        float m = soft_clip(0.5f * (l + r));
        ctx->data[at] = m;
        if (ctx->rec_armed) {
            RecRing_push(&a->rec, m);
            RecRing_push(&a->rec, m);
        }
    } else {
        float cl = soft_clip(l), cr = soft_clip(r);
        ctx->data[at] = cl;
        ctx->data[at + 1] = cr;
        for (int c = 2; c < ctx->channels; c++) ctx->data[at + c] = 0.0f;
        if (ctx->rec_armed) {
            RecRing_push(&a->rec, cl);
            RecRing_push(&a->rec, cr);
        }
    }
}

static void render(void *ud, float *data, size_t frames, int channels) {
    AudioState *s = ud;
    App *a = s->app;
    struct timespec began;
    clock_gettime(CLOCK_MONOTONIC, &began);

    Event ev;
    while (EventRing_pop(&a->ctrl, &ev)) {
        if (ev.kind == EV_ENGAGE) s->engaged = ev.u.flag;
        if (ev.kind == EV_RECORD) s->rec_on = ev.u.flag;
        if (ev.kind == EV_SET_MIDI_DRIVING) s->midi_driving = ev.u.flag;
        engine_apply(s, ev);
    }
    while (EventRing_pop(&a->midi_ev, &ev)) engine_apply(s, ev);

    bool rec_armed = s->rec_on;
    size_t done = 0;
    while (done < frames) {
        size_t until;
        if (melody_samples_until_fire(&s->melody, &until) && until == 0) {
            float hz = melody_fire(&s->melody);
            if (!s->midi_driving) {
                voice_bank_note_off_all(&s->voice);
                float vel =
                    velocity_for_level(voice_bank_patch(&s->voice)->master_level);
                voice_bank_note_on(&s->voice, -1, hz, vel);
                chandas_note_pulse(&s->chandas);
                mod_note_on(&s->mod);
            }
        }
        size_t run = frames - done;
        if (melody_samples_until_fire(&s->melody, &until) && until < run)
            run = until;
        bool modulating = mod_any_lfo(&s->mod) || s->mod.groups_prev;
        if (modulating && run > MOD_BLOCK) run = MOD_BLOCK;
        if (run < 1) run = 1;
        if (modulating) mod_tick(s, run);
        RenderCtx ctx = {s, data, done, channels, rec_armed,
                         voice_bank_chain(&s->voice)->amp.kind == AMP_ENVELOPE};
        voice_bank_render_frames(&s->voice, run, emit_frame, &ctx);
        melody_advance(&s->melody, run);
        done += run;
    }
    lfo_meter_store(&a->lfo_meter, &s->mod);

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
    chandas_init(&s->chandas, a->sample_rate);
    tape_init(&s->tape, a->sample_rate);
    mod_init(&s->mod, a->sample_rate);
    s->base.patch = a->shadow;
    s->base.verb = a->shadow_verb;
    s->base.chandas = chandas_params(&s->chandas);
    s->base.melody = melody_params_default();
    s->base.warmth = tape_warmth(&s->tape);
    s->base.bend = 0.0f;
    s->engaged = true;
    engage_gate_init(&s->gate, a->sample_rate, s->engaged);
    midi_note_clear(&a->midi_note);
    return audio_out_start(&a->audio, render, s);
}

void gui_audio_stop(App *a) {
    audio_out_stop(&a->audio);
    chandas_free(&g_as.chandas);
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

void app_send(App *a, Event ev) {
    if (!EventRing_push(&a->ctrl, ev))
        push_log(a, "control ring full — a change was dropped.");
}

void app_send_mods(App *a) {
    for (int i = 0; i < MOD_LFOS; i++)
        app_send(a, (Event){.kind = EV_SET_LFO,
                            .u.lfo = {i, a->mods.lfo[i]}});
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
        a->lissa_head = (a->lissa_head + 1) % 512;
        if (a->lissa_len < 512) a->lissa_len++;
    }
}

void gui_sync_chain(App *a) {
    bool notes_drive = midi_driving(a) || a->shadow_melody.enabled || a->hosted;
    Chain want;
    if (notes_drive && !a->engaged) {
        want.amp.kind = AMP_ENVELOPE;
        want.amp.env = env_params_default();
        want.amp.env.attack_s = a->shadow_attack_s;
        want.amp.env.decay_s = a->shadow_decay_s;
        want.amp.env.sustain = a->shadow_sustain;
        want.amp.env.curve = a->shadow.curve;
        want.amp.env.release_s = a->shadow_release_s;
    } else {
        want = chain_default();
    }
    bool same = want.amp.kind == a->chain.amp.kind
                && (want.amp.kind != AMP_ENVELOPE
                    || (want.amp.env.attack_s == a->chain.amp.env.attack_s
                        && want.amp.env.decay_s == a->chain.amp.env.decay_s
                        && want.amp.env.release_s == a->chain.amp.env.release_s
                        && want.amp.env.curve == a->chain.amp.env.curve
                        && want.amp.env.sustain == a->chain.amp.env.sustain));
    if (same) return;
    bool becoming_notes = want.amp.kind == AMP_ENVELOPE;
    bool was_notes = a->chain.amp.kind == AMP_ENVELOPE;
    a->chain = want;
    Event ev = {.kind = EV_SET_CHAIN, .u.chain = want};
    app_send(a, ev);
    if (becoming_notes != was_notes)
        push_log(a, becoming_notes
                        ? "notes raise the sound now. velocity is the level; "
                          "the envelope under the keys shapes the rest."
                        : "the drone holds the sound again.");
}

void gui_apply_cc(App *a, Ui *ui) {
    (void)ui;
    bool patch = false, verb = false, warmth = false;
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
        float p = cc_position(word);
        switch (target) {
        case CC_INDEX: a->shadow.index = p; patch = true; break;
        case CC_RIP: a->shadow.rip = p; patch = true; break;
        case CC_FB: a->shadow.feedback = p; patch = true; break;
        case CC_FIELD: a->shadow.field = p; patch = true; break;
        case CC_CURVE: a->shadow.curve = p; patch = true; break;
        case CC_RELEASE:
            a->shadow_release_s = env_time_at(p, ENV_RELEASE_MIN);
            break;
        case CC_ATTACK: a->shadow_attack_s = env_time_at(p, 0.0f); break;
        case CC_ENVDECAY: a->shadow_decay_s = env_time_at(p, 0.0f); break;
        case CC_SUSTAIN: a->shadow_sustain = p; break;
        case CC_GLIDE:
            a->shadow.glide_seconds = 2.0f * powf(p, PHI * PHI * PHI * PHI);
            patch = true;
            break;
        case CC_DRONEHZ: {
            a->drone_hz = log_position(p, 27.5f, 440.0f);
            Event ev = {.kind = EV_GLIDE_TO, .u.f = a->drone_hz};
            app_send(a, ev);
            break;
        }
        case CC_MIX: a->shadow_verb.mix = p; verb = true; break;
        case CC_GHOST: a->shadow_verb.ghost = p; verb = true; break;
        case CC_DECAY:
            a->shadow_verb.decay = log_position(p, 0.05f, 8.0f);
            verb = true;
            break;
        case CC_DAMP: a->shadow_verb.damp = p * 0.99f; verb = true; break;
        case CC_HAUNT: a->shadow_verb.haunt = p; verb = true; break;
        case CC_WARMTH: a->shadow_warmth = p; warmth = true; break;
        case CC_NONE: break;
        }
    }
    Event ev;
    if (patch) {
        ev.kind = EV_SET_PATCH;
        ev.u.patch = a->shadow;
        app_send(a, ev);
    }
    if (verb) {
        ev.kind = EV_SET_VERB;
        ev.u.verb = a->shadow_verb;
        app_send(a, ev);
    }
    if (warmth) {
        ev.kind = EV_SET_WARMTH;
        ev.u.f = a->shadow_warmth;
        app_send(a, ev);
    }
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
    double now = a->last_frame_time;
    long t = (long)now;
    char line[LOG_LINE_LEN];
    int n = snprintf(line, sizeof line, "[%02ld:%02ld:%02ld] ", t / 3600,
                     t / 60 % 60, t % 60);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
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
        /* the timestamp goes on the first line; the rest indent under it */
        if (i == 0)
            push_log(a, "%s", v->line[i].text);
        else
            push_log(a, "           %s", v->line[i].text);
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
