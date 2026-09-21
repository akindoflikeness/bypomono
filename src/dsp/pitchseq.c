#include "dsp.h"
#include <math.h>
#include <stdint.h>

PitchSeqParams pitch_seq_params_default(void) {
    PitchSeqParams p = {0};
    p.enabled = false;
    p.division = PITCH_DEFAULT_DIVISION;
    p.length = PITCH_STEPS;
    p.root_midi = 45;
    p.snap = false;
    p.gate_len = 0.5f;
    for (int k = 0; k < PITCH_STEPS; k++) {
        p.gate[k] = true;
        p.velocity[k] = 1.0f;
    }
    return p;
}

PitchSeqParams pitch_seq_sanitize(PitchSeqParams p) {
    if (p.division < 0 || p.division >= CHANDAS_DIVISIONS_LEN)
        p.division = PITCH_DEFAULT_DIVISION;
    if (p.length < 1) p.length = 1;
    if (p.length > PITCH_STEPS) p.length = PITCH_STEPS;
    if (p.root_midi > 127) p.root_midi = 127;
    p.gate_len = clampf(isfinite(p.gate_len) ? p.gate_len : 0.5f,
                        PITCH_GATE_LEN_MIN, 1.0f);
    for (int k = 0; k < PITCH_STEPS; k++) {
        float st = isfinite(p.pitch[k]) ? p.pitch[k] : 0.0f;
        p.pitch[k] = clampf(st, -PITCH_RANGE_ST, PITCH_RANGE_ST);
        float v = isfinite(p.velocity[k]) ? p.velocity[k] : 1.0f;
        p.velocity[k] = clampf(v, 0.0f, 1.0f);
    }
    return p;
}

float pitch_seq_semitones(const PitchSeqParams *p, int step) {
    float st = p->pitch[step % PITCH_STEPS];
    return p->snap ? roundf(st) : st;
}

float pitch_seq_step_seconds(const PitchSeqParams *p, float bpm) {
    int d = p->division >= 0 && p->division < CHANDAS_DIVISIONS_LEN
                ? p->division : PITCH_DEFAULT_DIVISION;
    return CHANDAS_DIVISIONS[d].beats * 60.0f
           / clampf(bpm, CHANDAS_MIN_BPM, CHANDAS_MAX_BPM);
}

static size_t step_samples(const PitchSeq *s) {
    size_t n = (size_t)(pitch_seq_step_seconds(&s->params, s->bpm) * s->sample_rate);
    return n < 2 ? 2 : n;
}

void pitch_seq_init(PitchSeq *s, float sample_rate) {
    s->params = pitch_seq_params_default();
    s->sample_rate = fmaxf(sample_rate, 1.0f);
    s->bpm = CHANDAS_DEFAULT_BPM;
    s->step = -1;
    s->until_step = 0;
    s->until_off = 0;
    s->held = false;
}

void pitch_seq_set_params(PitchSeq *s, PitchSeqParams p) {
    bool starting = p.enabled && !s->params.enabled;
    s->params = pitch_seq_sanitize(p);
    if (starting) {
        s->step = -1;
        s->until_step = 0;
    }
    size_t n = step_samples(s);
    if (s->until_step > n) s->until_step = n;
}

void pitch_seq_set_tempo(PitchSeq *s, float bpm) {
    s->bpm = bpm;
    size_t n = step_samples(s);
    if (s->until_step > n) s->until_step = n;
}

bool pitch_seq_samples_until(const PitchSeq *s, size_t *out) {
    /* a note still held when the sequencer stops is let go straight away */
    if (!s->params.enabled) {
        if (!s->held) return false;
        *out = 0;
        return true;
    }
    *out = s->held && s->until_off < s->until_step ? s->until_off : s->until_step;
    return true;
}

PitchEvent pitch_seq_fire(PitchSeq *s) {
    PitchEvent e = {PITCH_EV_NONE, 0.0f, 0.0f};
    if (s->held && (!s->params.enabled || s->until_off == 0)) {
        s->held = false;
        e.kind = PITCH_EV_OFF;
        return e;
    }
    if (!s->params.enabled || s->until_step != 0) return e;
    s->step = (s->step + 1) % s->params.length;
    size_t n = step_samples(s);
    s->until_step = n;
    if (!s->params.gate[s->step]) return e;
    /* a full-length gate ends on the next step's start, so a held note is
       let go before the next one and every gated step is heard */
    size_t off = (size_t)(s->params.gate_len * (float)n);
    s->until_off = off < 1 ? 1 : (off > n ? n : off);
    s->held = true;
    e.kind = PITCH_EV_ON;
    float midi = (float)s->params.root_midi + pitch_seq_semitones(&s->params, s->step);
    e.hz = 440.0f * exp2f((midi - 69.0f) / 12.0f);
    e.velocity = s->params.velocity[s->step];
    return e;
}

void pitch_seq_advance(PitchSeq *s, size_t samples) {
    if (!s->params.enabled) return;
    s->until_step = samples >= s->until_step ? 0 : s->until_step - samples;
    if (s->held) s->until_off = samples >= s->until_off ? 0 : s->until_off - samples;
}

void pitch_event_play(PitchEvent e, VoiceBank *v, Chandas *h, Mod *m) {
    if (e.kind == PITCH_EV_OFF) {
        voice_bank_note_off_all(v);
    } else if (e.kind == PITCH_EV_ON) {
        voice_bank_note_off_all(v);
        voice_bank_note_on(v, -1, e.hz, e.velocity);
        chandas_note_pulse(h);
        mod_note_on(m);
    }
}
