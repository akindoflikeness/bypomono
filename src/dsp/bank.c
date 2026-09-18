#include "dsp.h"
#include <math.h>

#define UNISON_TRIM 0.70710678f /* two voices sum to the level of one */

static int count_in(int n, int hi) {
    return n < 1 ? 1 : (n > hi ? hi : n);
}

static VoicePair *slot_pair(VoiceBank *b, int note, int copy) {
    return &b->pairs[note * UNISON_MAX + copy];
}

static const VoicePair *slot_pair_c(const VoiceBank *b, int note, int copy) {
    return &b->pairs[note * UNISON_MAX + copy];
}

static int reach_unison(const VoiceBank *b) {
    return count_in(voice_bank_patch(b)->unison, UNISON_MAX);
}

/* the drone is one held tone, so only notes spread across the slots */
static int reach_poly(const VoiceBank *b) {
    if (voice_bank_chain(b)->amp.kind != AMP_ENVELOPE) return 1;
    return count_in(voice_bank_patch(b)->voices, POLY_MAX);
}

static bool slot_wanted(const VoiceBank *b, int s) {
    return s / UNISON_MAX < b->poly && s % UNISON_MAX < reach_unison(b);
}

/* a parked copy (unison off, faded out) is not part of the note */
static bool note_silent(const VoiceBank *b, int note) {
    int unison = reach_unison(b);
    for (int copy = 0; copy < UNISON_MAX; copy++) {
        if (copy >= unison && b->gain[note * UNISON_MAX + copy] == 0.0f) continue;
        if (!voice_pair_silent(slot_pair_c(b, note, copy))) return false;
    }
    return true;
}

static void release_note(VoiceBank *b, int note) {
    for (int copy = 0; copy < UNISON_MAX; copy++)
        voice_pair_note_off(slot_pair(b, note, copy));
    b->held[note] = false;
}

static void hush_pair(VoicePair *p) {
    for (int i = 0; i < 2; i++) envelope_init(&p->voices[i].env, p->voices[i].sample_rate);
}

/* a copy coming back in picks up where the note's first copy is */
static void sync_copy(VoicePair *dst, const VoicePair *src) {
    const Voice *from = &src->voices[src->target];
    for (int i = 0; i < 2; i++) {
        Voice *v = &dst->voices[i];
        v->freq = from->freq;
        v->target_freq = from->target_freq;
        v->master = from->master;
        v->master_pos = from->master_pos;
        v->index = from->index;
        v->field_smooth = from->field_smooth;
        v->curve_smooth = from->curve_smooth;
        v->field_amount = from->field_amount;
        v->field_pitch = from->field_pitch;
        v->bend = from->bend;
        v->rip_smooth = from->rip_smooth;
        v->fb_smooth = from->fb_smooth;
        for (int op = 0; op < NUM_OPS; op++) v->level_s[op] = from->level_s[op];
        v->env = from->env;
        v->breath = from->breath;
        voice_wake(v);
    }
}

static void apply_voicing(VoiceBank *b) {
    const Patch *p = voice_bank_patch(b);
    int unison = reach_unison(b);
    float cents = clampf(p->unison_detune, 0.0f, UNISON_DETUNE_MAX);
    for (int note = 0; note < POLY_MAX; note++) {
        voice_pair_set_detune_cents(slot_pair(b, note, 0), unison > 1 ? -0.5f * cents : 0.0f);
        voice_pair_set_detune_cents(slot_pair(b, note, 1), 0.5f * cents);
    }
    int poly = reach_poly(b);
    if (poly < b->poly) {
        for (int note = poly; note < POLY_MAX; note++) release_note(b, note);
    }
    b->poly = poly;
    if (b->newest >= poly) b->newest = 0;
}

void voice_bank_init(VoiceBank *b, float sample_rate, Patch patch) {
    for (int s = 0; s < BANK_PAIRS; s++) {
        voice_pair_init(&b->pairs[s], sample_rate, patch);
        /* the second copy starts a golden fraction of a cycle along, so the
           pair does not open phase-locked */
        float offset = fract_pos((float)(s % UNISON_MAX) / PHI);
        for (int i = 0; i < 2; i++)
            for (int op = 0; op < NUM_OPS; op++) b->pairs[s].voices[i].phase[op] = offset;
    }
    for (int n = 0; n < POLY_MAX; n++) {
        b->key[n] = -1;
        b->held[n] = false;
        b->stamp[n] = 0;
    }
    b->clock = 0;
    b->newest = 0;
    b->poly = POLY_MAX;
    apply_voicing(b);
    for (int s = 0; s < BANK_PAIRS; s++) b->gain[s] = slot_wanted(b, s) ? 1.0f : 0.0f;
    b->spread = reach_unison(b) > 1 ? 1.0f : 0.0f;
    b->step = glide_k(GATE_GLIDE_S, sample_rate);
}

void voice_bank_free(VoiceBank *b) {
    for (int s = 0; s < BANK_PAIRS; s++) voice_pair_free(&b->pairs[s]);
}

bool voice_bank_crossing(const VoiceBank *b) {
    return voice_pair_crossing(&b->pairs[0]);
}

State voice_bank_state(const VoiceBank *b) {
    return voice_pair_state(&b->pairs[0]);
}

void voice_bank_set_state(VoiceBank *b, State next) {
    for (int s = 0; s < BANK_PAIRS; s++) {
        VoicePair *p = &b->pairs[s];
        /* a slot nobody hears takes the change outright instead of spending
           a crossfade rendering silence */
        bool quiet = s > 0 && (b->gain[s] == 0.0f || (!voice_pair_crossing(p) && voice_pair_silent(p)));
        if (!quiet) {
            voice_pair_set_state(p, next);
            continue;
        }
        for (int i = 0; i < 2; i++) {
            voice_set_patch(&p->voices[i], next.patch);
            voice_set_chain(&p->voices[i], next.chain);
        }
        p->step = 0.0f;
        p->blend = (float)p->target;
        if (!voice_pair_silent(p)) b->gain[s] = 0.0f;
    }
    apply_voicing(b);
}

void voice_bank_set_patch(VoiceBank *b, Patch patch) {
    State s;
    s.patch = patch;
    s.chain = *voice_bank_chain(b);
    voice_bank_set_state(b, s);
}

void voice_bank_set_chain_now(VoiceBank *b, Chain chain) {
    for (int s = 0; s < BANK_PAIRS; s++) voice_pair_set_chain_now(&b->pairs[s], chain);
    apply_voicing(b);
    for (int s = 0; s < BANK_PAIRS; s++) b->gain[s] = slot_wanted(b, s) ? 1.0f : 0.0f;
}

const Patch *voice_bank_patch(const VoiceBank *b) {
    return voice_pair_patch(&b->pairs[0]);
}

const Compiled *voice_bank_compiled(const VoiceBank *b) {
    return voice_pair_compiled(&b->pairs[0]);
}

const Chain *voice_bank_chain(const VoiceBank *b) {
    return voice_pair_chain(&b->pairs[0]);
}

int voice_bank_poly(const VoiceBank *b) {
    return b->poly;
}

void voice_bank_set_freq_hz(VoiceBank *b, float hz) {
    for (int s = 0; s < BANK_PAIRS; s++) voice_pair_set_freq_hz(&b->pairs[s], hz);
}

void voice_bank_set_drone_hz(VoiceBank *b, float hz) {
    for (int s = 0; s < BANK_PAIRS; s++) voice_pair_set_drone_hz(&b->pairs[s], hz);
}

void voice_bank_glide_to_hz(VoiceBank *b, float hz) {
    for (int copy = 0; copy < UNISON_MAX; copy++) voice_pair_glide_to_hz(slot_pair(b, 0, copy), hz);
}

void voice_bank_drone_to_hz(VoiceBank *b, float hz) {
    for (int copy = 0; copy < UNISON_MAX; copy++)
        voice_pair_drone_to_hz(slot_pair(b, 0, copy), hz);
}

/* same key again, then a free slot, then the oldest released, then the
   oldest held */
static int pick_note(const VoiceBank *b, int key) {
    if (key >= 0) {
        for (int n = 0; n < b->poly; n++)
            if (b->key[n] == key && (b->held[n] || !note_silent(b, n))) return n;
    }
    for (int pass = 0; pass < 3; pass++) {
        int best = -1;
        for (int n = 0; n < b->poly; n++) {
            if (pass == 0 && (b->held[n] || !note_silent(b, n))) continue;
            if (pass == 1 && b->held[n]) continue;
            if (best < 0 || b->stamp[n] < b->stamp[best]) best = n;
        }
        if (best >= 0) return best;
    }
    return 0;
}

void voice_bank_note_on(VoiceBank *b, int key, float hz, float velocity) {
    int note = b->poly > 1 ? pick_note(b, key) : 0;
    /* mono glides from wherever it is; a fresh poly slot starts on its pitch */
    bool fresh = b->poly > 1 && note_silent(b, note);
    int unison = reach_unison(b);
    for (int copy = 0; copy < UNISON_MAX; copy++) {
        VoicePair *p = slot_pair(b, note, copy);
        int s = note * UNISON_MAX + copy;
        /* a parked copy stays idle; it syncs to the first copy if it returns */
        if (copy >= unison && b->gain[s] == 0.0f) continue;
        if (fresh) {
            voice_pair_set_freq_hz(p, hz);
            voice_pair_wake(p);
            if (copy < unison) b->gain[s] = 1.0f;
        }
        voice_pair_note_on(p, hz, velocity);
    }
    b->key[note] = key;
    b->held[note] = true;
    b->stamp[note] = ++b->clock;
    b->newest = note;
}

void voice_bank_note_off(VoiceBank *b, int key) {
    for (int n = 0; n < POLY_MAX; n++) {
        if (b->poly > 1 && key >= 0 && b->key[n] != key) continue;
        release_note(b, n);
    }
}

void voice_bank_note_off_all(VoiceBank *b) {
    for (int n = 0; n < POLY_MAX; n++) release_note(b, n);
}

bool voice_bank_note_sounding(const VoiceBank *b) {
    for (int n = 0; n < b->poly; n++)
        if (voice_pair_note_sounding(slot_pair_c(b, n, 0))) return true;
    return false;
}

void voice_bank_set_bend_semitones(VoiceBank *b, float semitones) {
    for (int s = 0; s < BANK_PAIRS; s++) voice_pair_set_bend_semitones(&b->pairs[s], semitones);
}

float voice_bank_target_hz(const VoiceBank *b) {
    return voice_pair_target_hz(slot_pair_c(b, b->newest, 0));
}

const Envelope *voice_bank_newest_env(const VoiceBank *b) {
    const VoicePair *p = slot_pair_c(b, b->newest, 0);
    return &p->voices[p->target].env;
}

int voice_bank_held_hz(const VoiceBank *b, float out[POLY_MAX]) {
    if (voice_bank_chain(b)->amp.kind != AMP_ENVELOPE) {
        out[0] = voice_pair_target_hz(slot_pair_c(b, 0, 0));
        return 1;
    }
    int count = 0;
    for (int n = 0; n < b->poly; n++)
        if (b->held[n]) out[count++] = voice_pair_target_hz(slot_pair_c(b, n, 0));
    return count;
}

typedef struct {
    Frame *buf;
    size_t i;
} Capture;

static void capture_emit(void *userdata, size_t n, const Frame *frame) {
    Capture *c = userdata;
    c->buf[c->i++] = *frame;
}

/* the house gate: a one-pole, because a linear ramp kinks at both ends and
   under dense playing those kinks are the crackle */
static float ramp(float x, float target, float k) {
    float next = glide_to(x, target, k);
    if (fabsf(target - next) < 1e-4f) next = target;
    return next;
}

void voice_bank_render_frames(VoiceBank *b, size_t count, FrameEmit emit, void *userdata) {
    size_t done = 0;
    while (done < count) {
        size_t run = BANK_CHUNK < count - done ? BANK_CHUNK : count - done;
        float target[BANK_PAIRS];
        int live[BANK_PAIRS];
        int n = 0;
        for (int s = 0; s < BANK_PAIRS; s++) {
            bool wanted = slot_wanted(b, s);
            target[s] = wanted ? 1.0f : 0.0f;
            VoicePair *p = &b->pairs[s];
            if (!wanted && b->gain[s] == 0.0f) continue;
            if (s / UNISON_MAX > 0 && !voice_pair_crossing(p) && voice_pair_silent(p)) continue;
            if (wanted && s % UNISON_MAX > 0 && b->gain[s] == 0.0f)
                sync_copy(p, &b->pairs[s - s % UNISON_MAX]);
            Capture c = { b->scratch[n], 0 };
            voice_pair_render_frames(p, run, capture_emit, &c);
            live[n++] = s;
        }
        float spread_to = reach_unison(b) > 1 ? 1.0f : 0.0f;

        /* one voice at full gain is the mono instrument, untouched */
        if (n == 1 && live[0] == 0 && b->gain[0] == 1.0f && b->spread == 0.0f && spread_to == 0.0f) {
            for (size_t k = 0; k < run; k++) emit(userdata, done + k, &b->scratch[0][k]);
            for (int s = 1; s < BANK_PAIRS; s++)
                for (size_t k = 0; k < run; k++)
                    b->gain[s] = ramp(b->gain[s], target[s], b->step);
            done += run;
            continue;
        }

        for (size_t k = 0; k < run; k++) {
            for (int s = 0; s < BANK_PAIRS; s++) b->gain[s] = ramp(b->gain[s], target[s], b->step);
            b->spread = ramp(b->spread, spread_to, b->step);
            float trim = 1.0f + b->spread * (UNISON_TRIM - 1.0f);
            float width = b->spread * UNISON_WIDTH;
            Frame f = { { 0.0f }, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            float loudest = 0.0f, low_hz = 0.0f;
            for (int j = 0; j < n; j++) {
                int s = live[j];
                const Frame *x = &b->scratch[j][k];
                float g = b->gain[s] * trim;
                float pan = s % UNISON_MAX ? width : -width;
                /* the room reads ops * master, so carry the weighted sum and
                   put the loudest voice's master back out */
                float w = g * x->master;
                for (int op = 0; op < NUM_OPS; op++) f.ops[op] += x->ops[op] * w;
                loudest = fmaxf(loudest, w);
                f.mix += g * x->mix;
                f.side += g * x->mix * pan;
                f.field = fmaxf(f.field, x->field);
                if (w > 0.0f && (low_hz == 0.0f || x->base_hz < low_hz)) low_hz = x->base_hz;
            }
            if (loudest > 0.0f)
                for (int op = 0; op < NUM_OPS; op++) f.ops[op] /= loudest;
            f.master = loudest;
            f.base_hz = low_hz > 0.0f ? low_hz : b->scratch[0][k].base_hz;
            emit(userdata, done + k, &f);
        }
        for (int j = 0; j < n; j++) {
            int s = live[j];
            if (target[s] == 0.0f && b->gain[s] == 0.0f) hush_pair(&b->pairs[s]);
        }
        done += run;
    }
}
