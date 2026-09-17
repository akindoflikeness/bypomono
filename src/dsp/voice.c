#include "dsp.h"
#include <math.h>
#include <stdlib.h>

#define MOD_SCALE TAU_F
#define FB_SCALE PI_F
#define RAMP_SECONDS 0.002f
#define PARAM_GLIDE_S 0.013f
#define RIP_DELAY_SECONDS (0.029f * PHI)
#define RIP_SCALE PI_F
#define RIP_MAX_FB 0.9f
#define FIELD_TO_INDEX 0.1f
#define RIP_DAMP (1.0f / (PHI * PHI))

static void rip_line_init(RipLine *r, float sample_rate, float hz) {
    size_t delay = (size_t)(RIP_DELAY_SECONDS * sample_rate);
    if (delay < 1) delay = 1;
    r->len = delay + 1;
    r->buf = (float *)calloc(r->len, sizeof(float));
    r->write = 0;
    r->delay = delay;
    r->rot_hz = hz;
    phase_rotator_init(&r->rot, allpass_coeff_for(hz, sample_rate, PHASE_PER_PASS));
    r->lp = 0.0f;
    r->fb = 0.0f;
}

static float rip_line_process(RipLine *r, float x) {
    size_t read = (r->write + r->len - r->delay) % r->len;
    float y = phase_rotator_process(&r->rot, r->buf[read]);
    r->lp = y + RIP_DAMP * (r->lp - y);
    r->buf[r->write] = x + r->lp * r->fb;
    r->write = (r->write + 1) % r->len;
    return r->lp;
}

static void rip_line_clear(RipLine *r) {
    for (size_t i = 0; i < r->len; i++) r->buf[i] = 0.0f;
    r->write = 0;
    phase_rotator_clear(&r->rot);
    r->lp = 0.0f;
}

void voice_init(Voice *v, float sample_rate, Patch patch) {
    v->sample_rate = sample_rate;
    v->patch = patch;
    v->compiled = compile(patch.algorithm);
    for (int i = 0; i < NUM_OPS; i++) {
        v->phase[i] = 0.0f;
        v->out[i] = 0.0f;
        v->gain[i] = patch.ops[i].enabled ? 1.0f : 0.0f;
        v->pending_reset[i] = false;
    }
    v->fb_hist[0] = 0.0f;
    v->fb_hist[1] = 0.0f;
    v->freq = 110.0f;
    v->target_freq = 110.0f;
    v->master = master_gain(patch.master_level);
    v->index = clampf(patch.index, 0.0f, 1.0f);
    rip_line_init(&v->rip_line, sample_rate, START_HZ);
    v->rip_sig = 0.0f;
    breath_init(&v->breath, sample_rate, patch.ratio_mode);
    v->field_pitch = 1.0f;
    v->bend = 1.0f;
    v->detune = 1.0f;
    v->chain = chain_default();
    envelope_init(&v->env, sample_rate);
    v->master_pos = clampf(patch.master_level, 0.0f, 1.0f);
    v->field_amount = 0.0f;
    v->field_smooth = clampf(patch.field, 0.0f, 1.0f);
    v->curve_smooth = clampf(patch.curve, 0.0f, 1.0f);
}

void voice_free(Voice *v) {
    free(v->rip_line.buf);
    v->rip_line.buf = NULL;
}

void voice_note_off(Voice *v) {
    breath_release(&v->breath);
    envelope_note_off(&v->env);
}

void voice_set_freq_hz(Voice *v, float hz) {
    v->freq = hz;
    v->target_freq = hz;
}

/* solving for the coefficient is a bisection, so only pay for it when the
   drone pitch actually moved */
void voice_set_drone_hz(Voice *v, float hz) {
    if (hz == v->rip_line.rot_hz) return;
    v->rip_line.rot_hz = hz;
    v->rip_line.rot.a = allpass_coeff_for(hz, v->sample_rate, PHASE_PER_PASS);
}

void voice_set_chain(Voice *v, Chain chain) {
    v->chain = chain;
}

void voice_note_on(Voice *v, float hz, float velocity) {
    voice_glide_to_hz(v, hz);
    envelope_note_on(&v->env, velocity);
}

bool voice_note_sounding(const Voice *v) {
    return envelope_active(&v->env);
}

void voice_set_bend_semitones(Voice *v, float semitones) {
    v->bend = exp2f(semitones / 12.0f);
}

void voice_set_detune_cents(Voice *v, float cents) {
    v->detune = exp2f(cents / 1200.0f);
}

void voice_wake(Voice *v) {
    rip_line_clear(&v->rip_line);
    v->rip_sig = 0.0f;
}

float voice_target_hz(const Voice *v) {
    return v->target_freq;
}

void voice_glide_to_hz(Voice *v, float hz) {
    v->target_freq = hz;
    breath_trigger(&v->breath);
}

static void reset_op(Voice *v, int op) {
    v->phase[op] = 0.0f;
    v->out[op] = 0.0f;
    if (op == v->compiled.feedback_op) {
        v->fb_hist[0] = 0.0f;
        v->fb_hist[1] = 0.0f;
    }
}

static void reset_all(Voice *v) {
    for (int i = 0; i < NUM_OPS; i++) {
        v->phase[i] = 0.0f;
        v->out[i] = 0.0f;
    }
    v->fb_hist[0] = 0.0f;
    v->fb_hist[1] = 0.0f;
    rip_line_clear(&v->rip_line);
    v->rip_sig = 0.0f;
}

void voice_set_op_enabled(Voice *v, int op, bool on) {
    if (on) {
        reset_op(v, op);
        v->pending_reset[op] = false;
        v->patch.ops[op].enabled = true;
    } else if (v->patch.ops[op].enabled) {
        v->patch.ops[op].enabled = false;
        v->pending_reset[op] = true;
    }
}

void voice_set_patch(Voice *v, Patch patch) {
    bool restructure = patch.algorithm != v->patch.algorithm;
    bool was_enabled[NUM_OPS];
    for (int i = 0; i < NUM_OPS; i++) was_enabled[i] = v->patch.ops[i].enabled;
    v->patch = patch;
    if (restructure) {
        v->compiled = compile(v->patch.algorithm);
        reset_all(v);
    }
    breath_set_mode(&v->breath, v->patch.ratio_mode);
    for (int op = 0; op < NUM_OPS; op++) {
        bool now_enabled = v->patch.ops[op].enabled;
        if (now_enabled) {
            if (!was_enabled[op]) {
                reset_op(v, op);
            }
            v->pending_reset[op] = false;
        } else if (was_enabled[op]) {
            v->pending_reset[op] = true;
        }
    }
}

void voice_set_algorithm(Voice *v, AlgorithmId algorithm) {
    Patch patch = v->patch;
    patch.algorithm = algorithm;
    voice_set_patch(v, patch);
}

float voice_op_phase(const Voice *v, int op) {
    return v->phase[op];
}

void voice_render_frames(Voice *v, size_t count, FrameEmit emit, void *userdata) {
    float glide = v->patch.glide_seconds > 0.0f
        ? expf(-1.0f / (v->patch.glide_seconds * v->sample_rate))
        : 0.0f;
    float ramp_step = 1.0f / (RAMP_SECONDS * v->sample_rate);
    float freq_mult[NUM_OPS];
    for (int i = 0; i < NUM_OPS; i++) {
        freq_mult[i] = v->patch.ops[i].ratio * exp2f(v->patch.ops[i].detune_cents / 1200.0f);
    }
    float level[NUM_OPS];
    float index_exp[NUM_OPS];
    for (int i = 0; i < NUM_OPS; i++) {
        uint8_t depth = v->compiled.depth[i];
        level[i] = v->patch.ops[i].level;
        index_exp[i] = depth > 1 ? powi_f(PHI, (int)depth - 2) : 0.0f;
    }
    float fb_exp = 1.0f / PHI;
    float index_target = clampf(v->patch.index, 0.0f, 1.0f);
    float master_target = master_gain(v->patch.master_level);
    float master_pos_target = clampf(v->patch.master_level, 0.0f, 1.0f);
    AmpSource amp = v->chain.amp;
    float field_target = clampf(v->patch.field, 0.0f, 1.0f);
    float curve_target = clampf(v->patch.curve, 0.0f, 1.0f);
    float param_k = 1.0f - expf(-1.0f / (PARAM_GLIDE_S * v->sample_rate));
    float inv_carriers = 1.0f / (float)v->compiled.carrier_count;
    float rip = clampf(v->patch.rip, 0.0f, 1.0f);
    v->rip_line.fb = RIP_MAX_FB * rip;
    float rip_amount = RIP_SCALE * rip;
    const int *eval_order = v->compiled.eval_order;

    for (size_t n = 0; n < count; n++) {
        v->freq = v->target_freq + (v->freq - v->target_freq) * glide;
        v->index += (index_target - v->index) * param_k;
        float index = clampf(v->index + v->field_amount * FIELD_TO_INDEX, 0.0f, 1.0f);
        float eff_level[NUM_OPS];
        for (int i = 0; i < NUM_OPS; i++) {
            eff_level[i] = level[i] * (index_exp[i] > 0.0f ? powf(index, index_exp[i]) : 1.0f);
        }
        float eff_feedback = v->patch.feedback * powf(index, fb_exp);

        float rip_pm = rip_amount * (v->rip_sig / (1.0f + fabsf(v->rip_sig)));

        for (int e = 0; e < NUM_OPS; e++) {
            int op = eval_order[e];
            bool enabled = v->patch.ops[op].enabled;

            float target = enabled ? 1.0f : 0.0f;
            float g = v->gain[op];
            v->gain[op] = g < target ? fminf(g + ramp_step, target)
                                     : fmaxf(g - ramp_step, target);

            if (!enabled && v->gain[op] == 0.0f) {
                if (v->pending_reset[op]) {
                    reset_op(v, op);
                    v->pending_reset[op] = false;
                }
                v->out[op] = 0.0f;
                continue;
            }

            float phase_mod = 0.0f;
            for (int m = 0; m < NUM_OPS; m++) {
                if ((v->compiled.modulators[op] >> m & 1) == 1) {
                    phase_mod += v->out[m];
                }
            }
            float angle = TAU_F * v->phase[op] + MOD_SCALE * phase_mod;
            if (op == v->compiled.feedback_op) {
                angle += FB_SCALE * eff_feedback * 0.5f * (v->fb_hist[0] + v->fb_hist[1]);
            }
            if ((v->compiled.carriers >> op & 1) == 1) {
                angle += rip_pm;
            }

            float osc = sinf(angle);
            float y = osc * eff_level[op] * v->gain[op];
            v->out[op] = y;
            if (op == v->compiled.feedback_op) {
                v->fb_hist[1] = v->fb_hist[0];
                v->fb_hist[0] = osc * level[op] * v->gain[op];
            }

            float phase = v->phase[op]
                + v->freq * v->bend * v->detune * v->field_pitch * freq_mult[op] / v->sample_rate;
            v->phase[op] = fract_pos(phase);
        }

        float mix = 0.0f;
        for (int op = 0; op < NUM_OPS; op++) {
            if ((v->compiled.carriers >> op & 1) == 1) {
                mix += v->out[op];
            }
        }
        v->rip_sig = rip_line_process(&v->rip_line, mix * inv_carriers);
        v->master += (master_target - v->master) * param_k;
        v->master_pos += (master_pos_target - v->master_pos) * param_k;
        EnvParams env_params = amp.kind == AMP_ENVELOPE ? amp.env : env_params_default();
        (void)envelope_tick(&v->env, &env_params);
        float floor_ = amp.kind == AMP_DRONE
            ? v->master
            : master_gain(envelope_amplitude(&v->env, v->master_pos));
        v->field_smooth += (field_target - v->field_smooth) * param_k;
        v->curve_smooth += (curve_target - v->curve_smooth) * param_k;
        Field field = breath_tick(&v->breath, v->freq, v->field_smooth, floor_, v->curve_smooth);
        v->field_pitch = field.pitch;
        v->field_amount = field.amount;
        Frame frame;
        for (int i = 0; i < NUM_OPS; i++) frame.ops[i] = v->out[i];
        frame.mix = mix * inv_carriers * field.gain;
        frame.master = field.gain;
        frame.field = field.amount;
        frame.base_hz = v->freq;
        frame.side = 0.0f;
        emit(userdata, n, &frame);
    }
}

static void voice_render_cb(void *userdata, size_t n, const Frame *frame) {
    ((float *)userdata)[n] = frame->mix;
}

void voice_render(Voice *v, float *buf, size_t len) {
    voice_render_frames(v, len, voice_render_cb, buf);
}
