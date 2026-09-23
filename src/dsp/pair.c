#include "dsp.h"
#include <math.h>

void voice_pair_init(VoicePair *p, float sample_rate, Patch patch) {
    voice_init(&p->voice, sample_rate, patch);
    p->sample_rate = sample_rate;
    p->dip = 1.0f;
    p->dip_len = (int)(STRUCT_DIP_SECONDS * sample_rate + 0.5f);
    if (p->dip_len < 1) p->dip_len = 1;
    p->dip_left = 0;
    p->dip_dir = 0;
    p->armed = false;
}

void voice_pair_free(VoicePair *p) {
    voice_free(&p->voice);
}

bool voice_pair_crossing(const VoicePair *p) {
    return p->dip_dir != 0 || p->armed;
}

State voice_pair_state(const VoicePair *p) {
    if (p->armed) return p->pending;
    State s;
    s.patch = p->voice.patch;
    s.adsr = p->voice.adsr;
    return s;
}

/* dip = 0.5*(1+cos(pi*u)) on the way down, the mirror on the way up.
   Both ends have a flat derivative, so the join is not a kink. */
static void advance_dip(VoicePair *p) {
    if (p->dip_dir == 0) return;
    if (p->dip_left > 0) p->dip_left--;
    float u = 1.0f - (float)p->dip_left / (float)p->dip_len;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    if (p->dip_dir < 0) p->dip = 0.5f * (1.0f + cosf(PI_F * u));
    else p->dip = 0.5f * (1.0f - cosf(PI_F * u));
    if (p->dip_left > 0) return;
    if (p->dip_dir < 0) {
        p->dip = 0.0f;
        if (p->armed) {
            voice_set_patch(&p->voice, p->pending.patch);
            voice_set_adsr(&p->voice, p->pending.adsr);
            voice_snap_ratios(&p->voice);
            p->armed = false;
        }
        p->dip_dir = 1;
        p->dip_left = p->dip_len;
        return;
    }
    p->dip = 1.0f;
    p->dip_dir = 0;
    p->dip_left = 0;
}

static void arm_dip(VoicePair *p, State next) {
    p->pending = next;
    p->pending_compiled = compile(next.patch.algorithm);
    p->armed = true;
    if (p->dip_dir < 0) return;
    float c = clampf(2.0f * p->dip - 1.0f, -1.0f, 1.0f);
    float u = acosf(c) / PI_F;
    int left = (int)((1.0f - u) * (float)p->dip_len + 0.5f);
    if (left < 1) left = 1;
    p->dip_dir = -1;
    p->dip_left = left;
}

void voice_pair_set_state(VoicePair *p, State next) {
    if (p->armed) {
        /* a repeated copy of the change, or a knob moved during the dip,
           rides along. Only a new shape turns the fade around. */
        bool again = state_is_structural_change(&p->pending, &next);
        p->pending = next;
        p->pending_compiled = compile(next.patch.algorithm);
        if (again) arm_dip(p, next);
        return;
    }
    bool sounding = voice_note_sounding(&p->voice) || p->dip < 1.0f;
    State cur = voice_pair_state(p);
    if (state_is_structural_change(&cur, &next) && sounding) {
        arm_dip(p, next);
        return;
    }
    voice_set_patch(&p->voice, next.patch);
    voice_set_adsr(&p->voice, next.adsr);
    if (!voice_note_sounding(&p->voice)) voice_snap_ratios(&p->voice);
}

void voice_pair_set_patch(VoicePair *p, Patch patch) {
    State s;
    s.patch = patch;
    s.adsr = p->armed ? p->pending.adsr : p->voice.adsr;
    voice_pair_set_state(p, s);
}

const Patch *voice_pair_patch(const VoicePair *p) {
    return p->armed ? &p->pending.patch : &p->voice.patch;
}

const Compiled *voice_pair_compiled(const VoicePair *p) {
    return p->armed ? &p->pending_compiled : &p->voice.compiled;
}

void voice_pair_set_op_enabled(VoicePair *p, int op, bool on) {
    voice_set_op_enabled(&p->voice, op, on);
}

void voice_pair_set_freq_hz(VoicePair *p, float hz) {
    voice_set_freq_hz(&p->voice, hz);
}

void voice_pair_set_drone_hz(VoicePair *p, float hz) {
    voice_set_drone_hz(&p->voice, hz);
}

void voice_pair_glide_to_hz(VoicePair *p, float hz) {
    voice_glide_to_hz(&p->voice, hz);
}

void voice_pair_drone_to_hz(VoicePair *p, float hz) {
    voice_drone_to_hz(&p->voice, hz);
}

void voice_pair_note_on(VoicePair *p, float hz, float velocity) {
    voice_note_on(&p->voice, hz, velocity);
}

void voice_pair_note_steal(VoicePair *p, float hz, float velocity) {
    voice_note_steal(&p->voice, hz, velocity);
}

bool voice_pair_note_sounding(const VoicePair *p) {
    return voice_note_sounding(&p->voice);
}

const EnvParams *voice_pair_adsr(const VoicePair *p) {
    return &p->voice.adsr;
}

void voice_pair_set_bend_semitones(VoicePair *p, float semitones) {
    voice_set_bend_semitones(&p->voice, semitones);
}

void voice_pair_note_off(VoicePair *p) {
    voice_note_off(&p->voice);
}

float voice_pair_target_hz(const VoicePair *p) {
    return voice_target_hz(&p->voice);
}

void voice_pair_set_adsr_now(VoicePair *p, EnvParams adsr) {
    voice_set_adsr(&p->voice, adsr);
    if (p->armed) p->pending.adsr = adsr;
}

void voice_pair_set_detune_cents(VoicePair *p, float cents) {
    voice_set_detune_cents(&p->voice, cents);
}

void voice_pair_wake(VoicePair *p) {
    voice_wake(&p->voice);
}

bool voice_pair_silent(const VoicePair *p) {
    return !envelope_active(&p->voice.env);
}

static void scale_frame(Frame *f, float g) {
    if (g >= 1.0f) return;
    f->master *= g;
    f->mix *= g;
    f->side *= g;
}

void voice_pair_render_block(VoicePair *p, Frame *out, size_t count) {
    size_t done = 0;
    while (done < count) {
        if (p->dip_dir == 0) {
            voice_render_block(&p->voice, out + done, count - done);
            return;
        }
        advance_dip(p);
        voice_render_block(&p->voice, out + done, 1);
        scale_frame(out + done, p->dip);
        done++;
    }
}
