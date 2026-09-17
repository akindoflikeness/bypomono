#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plug.h"

const ParamSpec PLUG_SPEC[P_COUNT] = {
    [P_DRONE] = {"DRONE", "", 0, 1, 0, K_ONOFF},
    [P_DRONE_HZ] = {"drone hz", "", 27.5, 440, 110, K_FLOAT},
    [P_ALGORITHM] = {"algorithm", "", 0, 7, 0, K_STEP},
    [P_RATIO_MODE] = {"ratio mode", "", 0, 4, 2, K_STEP},
    [P_INDEX] = {"INDEX", "", 0, 1, 1, K_FLOAT},
    [P_RIP] = {"RIP", "", 0, 1, 0, K_FLOAT},
    [P_FB] = {"fb", "", 0, 1, 0.5, K_FLOAT},
    [P_GLIDE] = {"glide s", "", 0, 2, 0.05, K_FLOAT},
    [P_FIELD] = {"field", "", 0, 1, 0, K_FLOAT},
    [P_CURVE] = {"curve", "", 0, 1, 0.5, K_FLOAT},
    [P_RELEASE] = {"release s", "", 0.05, 8, 2, K_FLOAT},
    [P_LEVEL] = {"level", "", 0, 1, 0.8, K_FLOAT},
    [P_OP1] = {"op 1", "operators", 0, 1, 1, K_ONOFF},
    [P_OP2] = {"op 2", "operators", 0, 1, 1, K_ONOFF},
    [P_OP3] = {"op 3", "operators", 0, 1, 1, K_ONOFF},
    [P_OP4] = {"op 4", "operators", 0, 1, 1, K_ONOFF},
    [P_OP5] = {"op 5", "operators", 0, 1, 1, K_ONOFF},
    [P_MIX] = {"mix", "room", 0, 1, 0.35, K_FLOAT},
    [P_GHOST] = {"ghost", "room", 0, 1, 0.4, K_FLOAT},
    [P_DECAY] = {"decay s", "room", 0.05, 8, 4, K_FLOAT},
    [P_DAMP] = {"damp", "room", 0, 0.99, 0.4, K_FLOAT},
    [P_HAUNT] = {"haunt", "room", 0, 1, 0, K_FLOAT},
    [P_CH_ON] = {"chandas", "chandas", 0, 1, 0, K_ONOFF},
    [P_CH_MIX] = {"mix", "chandas", 0, 1, 0.35, K_FLOAT},
    [P_CH_SYNC] = {"sync", "chandas", 0, 1, 1, K_ONOFF},
    [P_CH_DIV] = {"division", "chandas", 0, 24, 12, K_STEP},
    [P_CH_RATE] = {"rate hz", "chandas", 0.1, 8, 1, K_FLOAT},
    [P_CH_SPREAD] = {"spread", "chandas", 0, 1, 0.35, K_FLOAT},
    [P_CH_SIZE] = {"size", "chandas", 0.1, 2, 0.75, K_FLOAT},
    [P_CH_WARP] = {"warp", "chandas", 0, 1, 0, K_FLOAT},
    [P_CH_DIM] = {"dimension", "chandas", 0, 1, 0, K_FLOAT},
    [P_CH_TAIL] = {"tail", "chandas", 0, 1, 0, K_FLOAT},
    [P_SH_ON] = {"s&h", "melody", 0, 1, 0, K_ONOFF},
    [P_SH_SRC] = {"source", "melody", 0, 1, 0, K_STEP},
    [P_SH_TUNING] = {"tuning", "melody", 0, 4, 0, K_STEP},
    [P_SH_SCALE] = {"scale", "melody", 0, 7, 0, K_STEP},
    [P_SH_ROOT] = {"root", "melody", 24, 57, 45, K_STEP},
    [P_SH_RANGE] = {"range", "melody", 1, 13, 8, K_STEP},
    [P_SH_RATE] = {"rate hz", "melody", 0.1, 8, 1.618034, K_FLOAT},
    [P_WARMTH] = {"warmth", "", 0, 1, 0.5, K_FLOAT},
    [P_POLY] = {"poly", "voices", 0, 1, 0, K_ONOFF},
    [P_UNISON] = {"unison", "voices", 0, 1, 0, K_ONOFF},
    [P_DETUNE] = {"detune ct", "voices", 0, UNISON_DETUNE_MAX,
                  UNISON_DETUNE_DEFAULT, K_FLOAT},
    [P_ATTACK] = {"attack s", "envelope", 0, ENV_TIME_MAX, 0.008, K_FLOAT},
    [P_ENV_DECAY] = {"decay s", "envelope", 0, ENV_TIME_MAX, 2, K_FLOAT},
    [P_SUSTAIN] = {"sustain", "envelope", 0, 1, 1, K_FLOAT},
};

double plug_getv(const Plug *p, int id) {
    return atomic_load_explicit(&((Plug *)p)->vals[id], memory_order_relaxed);
}

void plug_setv(Plug *p, int id, double v) {
    const ParamSpec *s = &PLUG_SPEC[id];
    if (isnan(v)) v = s->min; /* NaN fails both comparisons below */
    if (v < s->min) v = s->min;
    if (v > s->max) v = s->max;
    atomic_store_explicit(&p->vals[id], v, memory_order_relaxed);
}

/* a value read back is already inside the spec range, but the rounding cast
   is only defined if it is, so clamp again before converting */
static int plug_getv_int(const Plug *p, int id) {
    const ParamSpec *s = &PLUG_SPEC[id];
    double v = plug_getv(p, id);
    if (isnan(v)) v = s->min;
    if (v < s->min) v = s->min;
    if (v > s->max) v = s->max;
    return (int)lround(v);
}

#define getv plug_getv
#define setv plug_setv
#define SPEC PLUG_SPEC

static Patch patch_of_vals(const Plug *p) {
    int alg = plug_getv_int(p, P_ALGORITHM);
    RatioMode mode = (RatioMode)plug_getv_int(p, P_RATIO_MODE);
    Patch patch = patch_init(ALGORITHMS[alg & 7], mode);
    patch.feedback = (float)getv(p, P_FB);
    patch.index = (float)getv(p, P_INDEX);
    patch.rip = (float)getv(p, P_RIP);
    patch.master_level = (float)getv(p, P_LEVEL);
    patch.glide_seconds = (float)getv(p, P_GLIDE);
    patch.field = (float)getv(p, P_FIELD);
    patch.curve = (float)getv(p, P_CURVE);
    for (int i = 0; i < NUM_OPS; i++)
        patch.ops[i].enabled = getv(p, P_OP1 + i) > 0.5;
    patch.voices = getv(p, P_POLY) > 0.5 ? POLY_MAX : 1;
    patch.unison = getv(p, P_UNISON) > 0.5 ? UNISON_MAX : 1;
    patch.unison_detune = (float)getv(p, P_DETUNE);
    return patch;
}

static Chain chain_of_vals(const Plug *p) {
    Chain want;
    if (getv(p, P_DRONE) > 0.5) {
        want = chain_default();
    } else {
        want.amp.kind = AMP_ENVELOPE;
        want.amp.env = env_params_default();
        want.amp.env.attack_s = (float)getv(p, P_ATTACK);
        want.amp.env.decay_s = (float)getv(p, P_ENV_DECAY);
        want.amp.env.sustain = (float)getv(p, P_SUSTAIN);
        want.amp.env.curve = (float)getv(p, P_CURVE);
        want.amp.env.release_s = (float)getv(p, P_RELEASE);
    }
    return want;
}

/* audio thread: push the atomics into the engine */
static void apply_vals(Plug *p) {
    if (!p->engine_alive) return;
    voice_bank_set_patch(&p->voice, patch_of_vals(p));
    verb_configure(&p->verb, voice_bank_patch(&p->voice),
                   voice_bank_compiled(&p->voice));
    VerbParams vp = {(float)getv(p, P_MIX), (float)getv(p, P_GHOST),
                     (float)getv(p, P_DECAY), (float)getv(p, P_DAMP),
                     (float)getv(p, P_HAUNT)};
    verb_set_params(&p->verb, vp);
    ChandasParams cp = chandas_params_default();
    cp.enabled = getv(p, P_CH_ON) > 0.5;
    cp.mix = (float)getv(p, P_CH_MIX);
    cp.sync = getv(p, P_CH_SYNC) > 0.5;
    cp.division = (size_t)plug_getv_int(p, P_CH_DIV);
    cp.rate_hz = (float)getv(p, P_CH_RATE);
    cp.spread = (float)getv(p, P_CH_SPREAD);
    cp.size = (float)getv(p, P_CH_SIZE);
    cp.warp = (float)getv(p, P_CH_WARP);
    cp.dimension = (float)getv(p, P_CH_DIM);
    cp.tail = (float)getv(p, P_CH_TAIL);
    chandas_set_params(&p->chandas, cp);
    MelodyParams mp = melody_params_default();
    bool was_enabled = p->melody.params.enabled;
    mp.enabled = getv(p, P_SH_ON) > 0.5;
    mp.source = getv(p, P_SH_SRC) > 0.5 ? HOLD_XORSHIFT : HOLD_GOLDEN_WEYL;
    mp.tuning = (Tuning)plug_getv_int(p, P_SH_TUNING);
    mp.scale = (Scale)plug_getv_int(p, P_SH_SCALE);
    mp.root_midi = (uint8_t)plug_getv_int(p, P_SH_ROOT);
    mp.range_degrees = (uint8_t)plug_getv_int(p, P_SH_RANGE);
    mp.rate_hz = (float)getv(p, P_SH_RATE);
    if (was_enabled && !mp.enabled) voice_bank_note_off_all(&p->voice);
    melody_set_params(&p->melody, mp);
    tape_set(&p->tape, (float)getv(p, P_WARMTH));
    p->base.patch = *voice_pair_patch(&p->voice);
    p->base.verb = vp;
    p->base.chandas = cp;
    p->base.melody = mp;
    p->base.warmth = (float)getv(p, P_WARMTH);

    /* polite in the DAW: silent until notes unless DRONE is thrown */
    p->engaged = getv(p, P_DRONE) > 0.5;
    State st = voice_bank_state(&p->voice);
    st.chain = chain_of_vals(p);
    voice_bank_set_state(&p->voice, st);
    /* glide only when the hz itself moved: glide_to_hz retriggers the
       breath gesture and overrides a sounding note's pitch */
    float hz = (float)getv(p, P_DRONE_HZ);
    if (hz != p->applied_drone_hz) {
        p->applied_drone_hz = hz;
        voice_bank_glide_to_hz(&p->voice, hz);
        voice_bank_set_drone_hz(&p->voice, hz);
        verb_set_drone_hz(&p->verb, hz);
    }
    atomic_store_explicit(&p->dirty, false, memory_order_relaxed);
    atomic_store_explicit(&p->host_touched, true, memory_order_relaxed);
}

/* ---------- session <-> vals (state extension) ---------- */

Session plug_session_of_vals(const Plug *p) {
    Session s = session_default();
    s.patch = patch_of_vals(p);
    s.verb.mix = (float)getv(p, P_MIX);
    s.verb.ghost = (float)getv(p, P_GHOST);
    s.verb.decay = (float)getv(p, P_DECAY);
    s.verb.damp = (float)getv(p, P_DAMP);
    s.verb.haunt = (float)getv(p, P_HAUNT);
    s.melody.enabled = getv(p, P_SH_ON) > 0.5;
    s.melody.source = getv(p, P_SH_SRC) > 0.5 ? HOLD_XORSHIFT : HOLD_GOLDEN_WEYL;
    s.melody.tuning = (Tuning)plug_getv_int(p, P_SH_TUNING);
    s.melody.scale = (Scale)plug_getv_int(p, P_SH_SCALE);
    s.melody.root_midi = (uint8_t)plug_getv_int(p, P_SH_ROOT);
    s.melody.range_degrees = (uint8_t)plug_getv_int(p, P_SH_RANGE);
    s.melody.rate_hz = (float)getv(p, P_SH_RATE);
    s.drone_hz = (float)getv(p, P_DRONE_HZ);
    s.chandas.enabled = getv(p, P_CH_ON) > 0.5;
    s.chandas.mix = (float)getv(p, P_CH_MIX);
    s.chandas.sync = getv(p, P_CH_SYNC) > 0.5;
    s.chandas.division = (size_t)plug_getv_int(p, P_CH_DIV);
    s.chandas.rate_hz = (float)getv(p, P_CH_RATE);
    s.chandas.spread = (float)getv(p, P_CH_SPREAD);
    s.chandas.size = (float)getv(p, P_CH_SIZE);
    s.chandas.warp = (float)getv(p, P_CH_WARP);
    s.chandas.dimension = (float)getv(p, P_CH_DIM);
    s.chandas.tail = (float)getv(p, P_CH_TAIL);
    s.warmth = (float)getv(p, P_WARMTH);
    s.attack_s = (float)getv(p, P_ATTACK);
    s.decay_s = (float)getv(p, P_ENV_DECAY);
    s.sustain = (float)getv(p, P_SUSTAIN);
    s.release_s = (float)getv(p, P_RELEASE);
    s.drone = getv(p, P_DRONE) > 0.5;
    return s;
}

static void vals_of_session(Plug *p, const Session *s) {
    int alg = 0;
    for (int i = 0; i < 8; i++)
        if (ALGORITHMS[i] == s->patch.algorithm) alg = i;
    setv(p, P_ALGORITHM, alg);
    setv(p, P_RATIO_MODE, (double)s->patch.ratio_mode);
    setv(p, P_INDEX, s->patch.index);
    setv(p, P_RIP, s->patch.rip);
    setv(p, P_FB, s->patch.feedback);
    setv(p, P_GLIDE, s->patch.glide_seconds);
    setv(p, P_FIELD, s->patch.field);
    setv(p, P_CURVE, s->patch.curve);
    setv(p, P_LEVEL, s->patch.master_level);
    for (int i = 0; i < NUM_OPS; i++)
        setv(p, P_OP1 + i, s->patch.ops[i].enabled ? 1 : 0);
    setv(p, P_POLY, s->patch.voices > 1 ? 1 : 0);
    setv(p, P_UNISON, s->patch.unison > 1 ? 1 : 0);
    setv(p, P_DETUNE, s->patch.unison_detune);
    setv(p, P_MIX, s->verb.mix);
    setv(p, P_GHOST, s->verb.ghost);
    setv(p, P_DECAY, s->verb.decay);
    setv(p, P_DAMP, s->verb.damp);
    setv(p, P_HAUNT, s->verb.haunt);
    setv(p, P_DRONE_HZ, s->drone_hz);
    setv(p, P_CH_ON, s->chandas.enabled ? 1 : 0);
    setv(p, P_CH_MIX, s->chandas.mix);
    setv(p, P_CH_SYNC, s->chandas.sync ? 1 : 0);
    setv(p, P_CH_DIV, (double)s->chandas.division);
    setv(p, P_CH_RATE, s->chandas.rate_hz);
    setv(p, P_CH_SPREAD, s->chandas.spread);
    setv(p, P_CH_SIZE, s->chandas.size);
    setv(p, P_CH_WARP, s->chandas.warp);
    setv(p, P_CH_DIM, s->chandas.dimension);
    setv(p, P_CH_TAIL, s->chandas.tail);
    setv(p, P_SH_ON, s->melody.enabled ? 1 : 0);
    setv(p, P_SH_SRC, s->melody.source == HOLD_XORSHIFT ? 1 : 0);
    setv(p, P_SH_TUNING, (double)s->melody.tuning);
    setv(p, P_SH_SCALE, (double)s->melody.scale);
    setv(p, P_SH_ROOT, (double)s->melody.root_midi);
    setv(p, P_SH_RANGE, (double)s->melody.range_degrees);
    setv(p, P_SH_RATE, s->melody.rate_hz);
    setv(p, P_WARMTH, s->warmth);
    setv(p, P_ATTACK, s->attack_s);
    setv(p, P_ENV_DECAY, s->decay_s);
    setv(p, P_SUSTAIN, s->sustain);
    setv(p, P_RELEASE, s->release_s);
    setv(p, P_DRONE, s->drone ? 1 : 0);
    atomic_store_explicit(&p->dirty, true, memory_order_relaxed);
}

/* ---------- editor events (audio thread) ---------- */

static void mirror_patch_vals(Plug *p, const Patch *patch) {
    int alg = 0;
    for (int i = 0; i < 8; i++)
        if (ALGORITHMS[i] == patch->algorithm) alg = i;
    setv(p, P_ALGORITHM, alg);
    setv(p, P_RATIO_MODE, (double)patch->ratio_mode);
    setv(p, P_INDEX, patch->index);
    setv(p, P_RIP, patch->rip);
    setv(p, P_FB, patch->feedback);
    setv(p, P_GLIDE, patch->glide_seconds);
    setv(p, P_FIELD, patch->field);
    setv(p, P_CURVE, patch->curve);
    setv(p, P_LEVEL, patch->master_level);
    for (int i = 0; i < NUM_OPS; i++)
        setv(p, P_OP1 + i, patch->ops[i].enabled ? 1 : 0);
    setv(p, P_POLY, patch->voices > 1 ? 1 : 0);
    setv(p, P_UNISON, patch->unison > 1 ? 1 : 0);
    setv(p, P_DETUNE, patch->unison_detune);
}

static void apply_gui_event(Plug *p, Event ev) {
    switch (ev.kind) {
    case EV_SET_PATCH:
        p->base.patch = ev.u.patch;
        voice_pair_set_patch(&p->voice, ev.u.patch);
        verb_configure(&p->verb, voice_pair_patch(&p->voice),
                       voice_pair_compiled(&p->voice));
        mirror_patch_vals(p, &ev.u.patch);
        break;
    case EV_SET_LFO: mod_set_lfo(&p->mod, ev.u.lfo.slot, ev.u.lfo.p); break;
    case EV_SET_ROUTE:
        mod_set_route(&p->mod, ev.u.route.slot, ev.u.route.r);
        break;
    case EV_SET_VERB:
        p->base.verb = ev.u.verb;
        verb_set_params(&p->verb, ev.u.verb);
        setv(p, P_MIX, ev.u.verb.mix);
        setv(p, P_GHOST, ev.u.verb.ghost);
        setv(p, P_DECAY, ev.u.verb.decay);
        setv(p, P_DAMP, ev.u.verb.damp);
        setv(p, P_HAUNT, ev.u.verb.haunt);
        break;
    case EV_SET_MELODY:
        p->base.melody = ev.u.melody;
        if (p->melody.params.enabled && !ev.u.melody.enabled)
            voice_bank_note_off_all(&p->voice);
        melody_set_params(&p->melody, ev.u.melody);
        setv(p, P_SH_ON, ev.u.melody.enabled ? 1 : 0);
        setv(p, P_SH_SRC, ev.u.melody.source == HOLD_XORSHIFT ? 1 : 0);
        setv(p, P_SH_TUNING, (double)ev.u.melody.tuning);
        setv(p, P_SH_SCALE, (double)ev.u.melody.scale);
        setv(p, P_SH_ROOT, (double)ev.u.melody.root_midi);
        setv(p, P_SH_RANGE, (double)ev.u.melody.range_degrees);
        setv(p, P_SH_RATE, ev.u.melody.rate_hz);
        break;
    case EV_SET_CHANDAS:
        p->base.chandas = ev.u.chandas;
        chandas_set_params(&p->chandas, ev.u.chandas);
        setv(p, P_CH_ON, ev.u.chandas.enabled ? 1 : 0);
        setv(p, P_CH_MIX, ev.u.chandas.mix);
        setv(p, P_CH_SYNC, ev.u.chandas.sync ? 1 : 0);
        setv(p, P_CH_DIV, (double)ev.u.chandas.division);
        setv(p, P_CH_RATE, ev.u.chandas.rate_hz);
        setv(p, P_CH_SPREAD, ev.u.chandas.spread);
        setv(p, P_CH_SIZE, ev.u.chandas.size);
        setv(p, P_CH_WARP, ev.u.chandas.warp);
        setv(p, P_CH_DIM, ev.u.chandas.dimension);
        setv(p, P_CH_TAIL, ev.u.chandas.tail);
        break;
    case EV_SET_WARMTH:
        p->base.warmth = ev.u.f;
        tape_set(&p->tape, ev.u.f);
        setv(p, P_WARMTH, ev.u.f);
        break;
    case EV_RESET_CHANDAS: chandas_reset(&p->chandas); break;
    case EV_SET_TEMPO: chandas_set_tempo(&p->chandas, ev.u.f); break;
    case EV_GLIDE_TO:
        voice_bank_glide_to_hz(&p->voice, ev.u.f);
        voice_bank_set_drone_hz(&p->voice, ev.u.f);
        verb_set_drone_hz(&p->verb, ev.u.f);
        p->applied_drone_hz = ev.u.f;
        setv(p, P_DRONE_HZ, ev.u.f);
        break;
    case EV_NOTE_OFF: voice_pair_note_off(&p->voice); break;
    case EV_BEND:
        p->base.bend = ev.u.f;
        voice_pair_set_bend_semitones(&p->voice, ev.u.f);
        break;
    case EV_NOTE_ON:
        voice_bank_note_on(&p->voice, ev.u.note.key, ev.u.note.hz,
                           ev.u.note.velocity);
        chandas_note_pulse(&p->chandas);
        mod_note_on(&p->mod);
        break;
    case EV_SET_CHAIN: {
        State next = voice_bank_state(&p->voice);
        next.chain = ev.u.chain;
        voice_bank_set_state(&p->voice, next);
        if (ev.u.chain.amp.kind == AMP_ENVELOPE) {
            setv(p, P_ATTACK, ev.u.chain.amp.env.attack_s);
            setv(p, P_ENV_DECAY, ev.u.chain.amp.env.decay_s);
            setv(p, P_SUSTAIN, ev.u.chain.amp.env.sustain);
            setv(p, P_CURVE, ev.u.chain.amp.env.curve);
            setv(p, P_RELEASE, ev.u.chain.amp.env.release_s);
        }
        break;
    }
    case EV_ENGAGE:
        p->engaged = ev.u.flag;
        setv(p, P_DRONE, ev.u.flag ? 1 : 0);
        break;
    case EV_RECORD:
        p->rec_on = ev.u.flag;
        break;
    case EV_SET_MIDI_DRIVING:
        break;
    }
}

/* ---------- rendering ---------- */

typedef struct {
    Plug *p;
    App *gapp;
    float *l, *r;
    uint32_t base;
} Emit;

static void emit_frame(void *ud, size_t n, const Frame *frame) {
    Emit *e = ud;
    Plug *p = e->p;
    Stereo w = verb_process(&p->verb, frame);
    w = chandas_process(&p->chandas, w);
    w = tape_process(&p->tape, w);
    bool notes_live = voice_bank_chain(&p->voice)->amp.kind == AMP_ENVELOPE;
    float g = engage_gate_next(&p->gate, p->engaged || notes_live);
    float l = soft_clip(w.l * g), r = soft_clip(w.r * g);
    e->l[e->base + n] = l;
    e->r[e->base + n] = r;
    App *gapp = e->gapp;
    if (gapp) {
        p->viz_decim++;
        p->peak_acc[0] = fmaxf(p->peak_acc[0], fabsf(l));
        p->peak_acc[1] = fmaxf(p->peak_acc[1], fabsf(r));
        if (p->viz_decim >= VIZ_DECIMATE) {
            p->viz_decim = 0;
            VizFrame vf;
            memcpy(vf.ops, frame->ops, sizeof vf.ops);
            vf.l = l;
            vf.r = r;
            vf.peak[0] = p->peak_acc[0];
            vf.peak[1] = p->peak_acc[1];
            VizRing_push(&gapp->viz, vf);
            p->peak_acc[0] = p->peak_acc[1] = 0.0f;
        }
        if (p->rec_on) {
            RecRing_push(&gapp->rec, l);
            RecRing_push(&gapp->rec, r);
        }
    }
}

/* writes base plus modulation into the engine for the groups that need it */
static void mod_tick(Plug *p, size_t samples) {
    mod_advance(&p->mod, samples, chandas_tempo(&p->chandas));
    ModBase out;
    int g = mod_apply(&p->mod, &p->base, &out);
    if (g & MOD_G_PATCH) voice_pair_set_patch(&p->voice, out.patch);
    if (g & MOD_G_VERB) verb_set_params(&p->verb, out.verb);
    if (g & MOD_G_CHANDAS) chandas_set_params(&p->chandas, out.chandas);
    if (g & MOD_G_MELODY) melody_set_params(&p->melody, out.melody);
    if (g & MOD_G_WARMTH) tape_set(&p->tape, out.warmth);
    if (g & MOD_G_BEND) voice_pair_set_bend_semitones(&p->voice, out.bend);
}

static void render_span(Plug *p, App *gapp, float *l, float *r, uint32_t base,
                        uint32_t count) {
    uint32_t done = 0;
    while (done < count) {
        size_t until;
        if (melody_samples_until_fire(&p->melody, &until) && until == 0) {
            float hz = melody_fire(&p->melody);
            voice_bank_note_off_all(&p->voice);
            float vel =
                velocity_for_level(voice_bank_patch(&p->voice)->master_level);
            voice_bank_note_on(&p->voice, -1, hz, vel);
            chandas_note_pulse(&p->chandas);
            mod_note_on(&p->mod);
        }
        uint32_t run = count - done;
        if (melody_samples_until_fire(&p->melody, &until) && until < run)
            run = (uint32_t)until;
        bool modulating = mod_any_lfo(&p->mod) || p->mod.groups_prev;
        if (modulating && run > MOD_BLOCK) run = MOD_BLOCK;
        if (run < 1) run = 1;
        if (modulating) mod_tick(p, run);
        Emit e = {p, gapp, l, r, base + done};
        voice_bank_render_frames(&p->voice, run, emit_frame, &e);
        melody_advance(&p->melody, run);
        done += run;
    }
}

static void handle_event(Plug *p, const clap_event_header_t *hdr) {
    if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    switch (hdr->type) {
    case CLAP_EVENT_NOTE_ON: {
        const clap_event_note_t *ev = (const clap_event_note_t *)hdr;
        if (ev->key < 0 || ev->key > 127) break;
        voice_bank_note_on(&p->voice, ev->key, midi_to_hz((uint8_t)ev->key),
                           (float)ev->velocity);
        chandas_note_pulse(&p->chandas);
        mod_note_on(&p->mod);
        break;
    }
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
        /* key -1 is the CLAP wildcard: every note */
        const clap_event_note_t *ev = (const clap_event_note_t *)hdr;
        voice_bank_note_off(&p->voice, ev->key < 0 ? -1 : ev->key);
        break;
    }
    case CLAP_EVENT_PARAM_VALUE: {
        const clap_event_param_value_t *ev =
            (const clap_event_param_value_t *)hdr;
        if (ev->param_id < P_COUNT) {
            setv(p, (int)ev->param_id, ev->value);
            apply_vals(p);
        }
        break;
    }
    case CLAP_EVENT_MIDI: {
        const clap_event_midi_t *ev = (const clap_event_midi_t *)hdr;
        uint8_t status = ev->data[0] & 0xF0;
        if (status == 0x90 && ev->data[2] > 0) {
            voice_bank_note_on(&p->voice, ev->data[1] & 0x7f,
                               midi_to_hz(ev->data[1] & 0x7f),
                               (float)ev->data[2] / 127.0f);
            chandas_note_pulse(&p->chandas);
            mod_note_on(&p->mod);
        } else if (status == 0x80 || status == 0x90) {
            voice_bank_note_off(&p->voice, ev->data[1] & 0x7f);
        } else if (status == 0xE0) {
            int raw = ((ev->data[2] & 0x7f) << 7) | (ev->data[1] & 0x7f);
            p->base.bend = (float)(raw - 8192) / 8192.0f * BEND_SEMITONES;
            voice_pair_set_bend_semitones(&p->voice, p->base.bend);
        }
        break;
    }
    default:
        break;
    }
}

static clap_process_status plug_process(const clap_plugin_t *plugin,
                                        const clap_process_t *pr) {
    Plug *p = plugin->plugin_data;
    if (!p->engine_alive || pr->audio_outputs_count < 1
        || pr->audio_outputs[0].channel_count < 2
        || !pr->audio_outputs[0].data32)
        return CLAP_PROCESS_ERROR;
    float *l = pr->audio_outputs[0].data32[0];
    float *r = pr->audio_outputs[0].data32[1];

    if (pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO))
        chandas_set_tempo(&p->chandas, (float)pr->transport->tempo);

    App *gapp = atomic_load_explicit(&p->gui_app, memory_order_acquire);
    Event ev;
    while (EventRing_pop(&p->mod_ev, &ev)) apply_gui_event(p, ev);
    if (gapp)
        while (EventRing_pop(&gapp->ctrl, &ev)) apply_gui_event(p, ev);

    if (atomic_load_explicit(&p->dirty, memory_order_relaxed)) apply_vals(p);

    const clap_input_events_t *in = pr->in_events;
    uint32_t n_ev = in ? in->size(in) : 0;
    uint32_t idx = 0, frame = 0;
    while (frame < pr->frames_count) {
        uint32_t next = pr->frames_count;
        while (idx < n_ev) {
            const clap_event_header_t *hdr = in->get(in, idx);
            if (hdr->time > frame) {
                next = hdr->time < pr->frames_count ? hdr->time
                                                    : pr->frames_count;
                break;
            }
            handle_event(p, hdr);
            idx++;
        }
        if (next > frame) {
            render_span(p, gapp, l, r, frame, next - frame);
            frame = next;
        }
    }
    while (idx < n_ev) handle_event(p, in->get(in, idx++));
    if (gapp) {
        pitch_store(&gapp->pitch, voice_pair_target_hz(&p->voice));
        lfo_meter_store(&gapp->lfo_meter, &p->mod);
    }
    return CLAP_PROCESS_CONTINUE;
}

/* ---------- extensions ---------- */

static uint32_t aports_count(const clap_plugin_t *pl, bool is_input) {
    return is_input ? 0 : 1;
}

static bool aports_get(const clap_plugin_t *pl, uint32_t index, bool is_input,
                       clap_audio_port_info_t *info) {
    if (is_input || index > 0) return false;
    info->id = 0;
    snprintf(info->name, sizeof info->name, "out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

static const clap_plugin_audio_ports_t EXT_AUDIO_PORTS = {aports_count,
                                                          aports_get};

static uint32_t nports_count(const clap_plugin_t *pl, bool is_input) {
    return is_input ? 1 : 0;
}

static bool nports_get(const clap_plugin_t *pl, uint32_t index, bool is_input,
                       clap_note_port_info_t *info) {
    if (!is_input || index > 0) return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    snprintf(info->name, sizeof info->name, "notes");
    return true;
}

static const clap_plugin_note_ports_t EXT_NOTE_PORTS = {nports_count,
                                                        nports_get};

static uint32_t params_count(const clap_plugin_t *pl) { return P_COUNT; }

static bool params_get_info(const clap_plugin_t *pl, uint32_t index,
                            clap_param_info_t *info) {
    if (index >= P_COUNT) return false;
    const ParamSpec *s = &SPEC[index];
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (s->kind != K_FLOAT) info->flags |= CLAP_PARAM_IS_STEPPED;
    info->cookie = NULL;
    snprintf(info->name, sizeof info->name, "%s", s->name);
    snprintf(info->module, sizeof info->module, "%s", s->module);
    info->min_value = s->min;
    info->max_value = s->max;
    info->default_value = s->def;
    return true;
}

static bool params_get_value(const clap_plugin_t *pl, clap_id id, double *out) {
    Plug *p = pl->plugin_data;
    if (id >= P_COUNT) return false;
    *out = getv(p, (int)id);
    return true;
}

static bool params_value_to_text(const clap_plugin_t *pl, clap_id id,
                                 double value, char *out, uint32_t size) {
    if (id >= P_COUNT) return false;
    const ParamSpec *ps = &PLUG_SPEC[id];
    if (isnan(value)) value = ps->min;
    if (value < ps->min) value = ps->min;
    if (value > ps->max) value = ps->max;
    int v = (int)lround(value);
    switch (id) {
    case P_ALGORITHM: {
        snprintf(out, size, "%s", ROMAN[v & 7]);
        return true;
    }
    case P_RATIO_MODE:
        snprintf(out, size, "%s", mode_name_of((RatioMode)(v < 0 ? 0 : v % 5)));
        return true;
    case P_SH_SCALE:
        snprintf(out, size, "%s", scale_name((Scale)(v & 7)));
        return true;
    case P_SH_TUNING:
        snprintf(out, size, "%s", tuning_name((Tuning)(v < 0 ? 0 : v % 5)));
        return true;
    case P_SH_SRC:
        snprintf(out, size, "%s", v ? "random" : "golden");
        return true;
    case P_POLY:
        snprintf(out, size, "%s", v ? "4 voices" : "mono");
        return true;
    case P_UNISON:
        snprintf(out, size, "%s", v ? "2 voices" : "off");
        return true;
    case P_CH_DIV:
        snprintf(out, size, "%s",
                 CHANDAS_DIVISIONS[v < 0 ? 0 : (v > 24 ? 24 : v)].name);
        return true;
    case P_DRONE:
    case P_OP1: case P_OP2: case P_OP3: case P_OP4: case P_OP5:
    case P_CH_ON: case P_CH_SYNC: case P_SH_ON:
        snprintf(out, size, "%s", v ? "on" : "off");
        return true;
    default:
        return false;
    }
}

static bool params_text_to_value(const clap_plugin_t *pl, clap_id id,
                                 const char *text, double *out) {
    return false;
}

static void params_flush(const clap_plugin_t *pl,
                         const clap_input_events_t *in,
                         const clap_output_events_t *out) {
    Plug *p = pl->plugin_data;
    uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; i++) {
        const clap_event_header_t *hdr = in->get(in, i);
        if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID
            || hdr->type != CLAP_EVENT_PARAM_VALUE)
            continue;
        const clap_event_param_value_t *ev =
            (const clap_event_param_value_t *)hdr;
        if (ev->param_id < P_COUNT) setv(p, (int)ev->param_id, ev->value);
    }
    if (p->engine_alive)
        apply_vals(p);
    else
        atomic_store_explicit(&p->dirty, true, memory_order_relaxed);
}

static const clap_plugin_params_t EXT_PARAMS = {
    params_count, params_get_info, params_get_value,
    params_value_to_text, params_text_to_value, params_flush};

static bool state_save(const clap_plugin_t *pl, const clap_ostream_t *stream) {
    Plug *p = pl->plugin_data;
    Session s = plug_session_of_vals(p);
    App *g = atomic_load_explicit(&p->gui_app, memory_order_acquire);
    s.mods = g ? g->mods : p->mods_main;
    s = session_sanitize(s);
    char *json = session_to_json(&s);
    if (!json) return false;
    size_t len = strlen(json), at = 0;
    while (at < len) {
        int64_t n = stream->write(stream, json + at, len - at);
        if (n <= 0) {
            free(json);
            return false;
        }
        at += (size_t)n;
    }
    free(json);
    return true;
}

static bool state_load(const clap_plugin_t *pl, const clap_istream_t *stream) {
    Plug *p = pl->plugin_data;
    size_t cap = 16384, len = 0;
    char *buf = malloc(cap);
    if (!buf) return false;
    for (;;) {
        if (len + 4096 + 1 > SESSION_JSON_MAX) {
            free(buf);
            return false;
        }
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return false;
            }
            buf = nb;
        }
        int64_t n = stream->read(stream, buf + len, 4096);
        if (n < 0 || n > 4096) {
            free(buf);
            return false;
        }
        if (n == 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    Session s;
    bool ok = session_from_json(buf, &s);
    free(buf);
    if (!ok) return false;
    vals_of_session(p, &s);
    p->mods_main = s.mods;
    for (int i = 0; i < MOD_LFOS; i++)
        EventRing_push(&p->mod_ev,
                       (Event){.kind = EV_SET_LFO, .u.lfo = {i, s.mods.lfo[i]}});
    for (int i = 0; i < MOD_ROUTES; i++)
        EventRing_push(&p->mod_ev, (Event){.kind = EV_SET_ROUTE,
                                           .u.route = {i, s.mods.route[i]}});
    App *g = atomic_load_explicit(&p->gui_app, memory_order_acquire);
    if (g) g->mods = s.mods;
    const clap_host_params_t *hp =
        p->host ? p->host->get_extension(p->host, CLAP_EXT_PARAMS) : NULL;
    if (hp) hp->rescan(p->host, CLAP_PARAM_RESCAN_VALUES);
    return true;
}

static const clap_plugin_state_t EXT_STATE = {state_save, state_load};

/* ---------- plugin lifecycle ---------- */

static bool plug_init(const clap_plugin_t *plugin) { return true; }

static void plug_destroy(const clap_plugin_t *plugin) {
    Plug *p = plugin->plugin_data;
    if (p->gui_state) PLUG_EXT_GUI.destroy(plugin);
    free(p);
}

static bool plug_activate(const clap_plugin_t *plugin, double sr,
                          uint32_t min_frames, uint32_t max_frames) {
    Plug *p = plugin->plugin_data;
    p->sr = sr;
    voice_bank_init(&p->voice, (float)sr, patch_of_vals(p));
    voice_bank_set_freq_hz(&p->voice, (float)getv(p, P_DRONE_HZ));
    p->applied_drone_hz = (float)getv(p, P_DRONE_HZ);
    /* born in the right chain: nothing to crossfade from on insert */
    voice_bank_set_chain_now(&p->voice, chain_of_vals(p));
    verb_init(&p->verb, (float)sr);
    voice_bank_set_drone_hz(&p->voice, (float)getv(p, P_DRONE_HZ));
    verb_set_drone_hz(&p->verb, (float)getv(p, P_DRONE_HZ));
    melody_init(&p->melody, (float)sr, melody_params_default());
    chandas_init(&p->chandas, (float)sr);
    tape_init(&p->tape, (float)sr);
    engage_gate_init(&p->gate, (float)sr, false);
    mod_init(&p->mod, (float)sr);
    p->mod.bank = mod_bank_sanitize(p->mods_main);
    p->base.bend = 0.0f;
    p->engine_alive = true;
    apply_vals(p);
    p->active = true;
    return true;
}

static void plug_deactivate(const clap_plugin_t *plugin) {
    Plug *p = plugin->plugin_data;
    if (p->engine_alive) {
        chandas_free(&p->chandas);
        verb_free(&p->verb);
        voice_bank_free(&p->voice);
        p->engine_alive = false;
    }
    p->active = false;
}

static bool plug_start_processing(const clap_plugin_t *plugin) { return true; }
static void plug_stop_processing(const clap_plugin_t *plugin) {}

static void plug_reset(const clap_plugin_t *plugin) {
    Plug *p = plugin->plugin_data;
    if (!p->engine_alive) return;
    voice_bank_note_off_all(&p->voice);
    chandas_reset(&p->chandas);
    tape_clear(&p->tape);
}

static const void *plug_get_extension(const clap_plugin_t *plugin,
                                      const char *id) {
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &EXT_AUDIO_PORTS;
    if (strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &EXT_NOTE_PORTS;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0) return &EXT_PARAMS;
    if (strcmp(id, CLAP_EXT_STATE) == 0) return &EXT_STATE;
    if (strcmp(id, CLAP_EXT_GUI) == 0) return &PLUG_EXT_GUI;
    if (strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) return &PLUG_EXT_TIMER;
#if BYPO_GUI_POSIX_FD
    if (strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT) == 0) return &PLUG_EXT_FD;
#endif
    return NULL;
}

static void plug_on_main_thread(const clap_plugin_t *plugin) {}

/* ---------- factory ---------- */

static const char *const FEATURES[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                       CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                       CLAP_PLUGIN_FEATURE_STEREO, NULL};

static const clap_plugin_descriptor_t DESC = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "com.akol.bypo",
    .name = "BYPO",
    .vendor = "A KIND OF LIKENESS",
    .url = "https://github.com/wraithsys/bypomono",
    .manual_url = "https://github.com/wraithsys/bypomono",
    .support_url = "https://github.com/wraithsys/bypomono",
    .version = "1.1.0",
    .description = "Blow Your Phase Off - Phase Violence PM Sound Design Synthesis",
    .features = FEATURES,
};

static const clap_plugin_t TEMPLATE = {
    .desc = &DESC,
    .plugin_data = NULL,
    .init = plug_init,
    .destroy = plug_destroy,
    .activate = plug_activate,
    .deactivate = plug_deactivate,
    .start_processing = plug_start_processing,
    .stop_processing = plug_stop_processing,
    .reset = plug_reset,
    .process = plug_process,
    .get_extension = plug_get_extension,
    .on_main_thread = plug_on_main_thread,
};

static uint32_t factory_count(const clap_plugin_factory_t *f) { return 1; }

static const clap_plugin_descriptor_t *
factory_desc(const clap_plugin_factory_t *f, uint32_t index) {
    return index == 0 ? &DESC : NULL;
}

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *f,
                                           const clap_host_t *host,
                                           const char *plugin_id) {
    if (strcmp(plugin_id, DESC.id) != 0) return NULL;
    Plug *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->plugin = TEMPLATE;
    p->plugin.plugin_data = p;
    p->host = host;
    p->sr = 48000.0;
    for (int i = 0; i < P_COUNT; i++)
        atomic_store_explicit(&p->vals[i], SPEC[i].def, memory_order_relaxed);
    return &p->plugin;
}

static const clap_plugin_factory_t FACTORY = {factory_count, factory_desc,
                                              factory_create};

static bool entry_init(const char *plugin_path) { return true; }
static void entry_deinit(void) {}

static const void *entry_get_factory(const char *factory_id) {
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0) return &FACTORY;
    return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = entry_init,
    .deinit = entry_deinit,
    .get_factory = entry_get_factory,
};
