#include "dsp.h"
#include <math.h>

static const float LEN[CHAMBER_N] = {
    1237.0f, 1601.0f, 1867.0f, 2179.0f, 2521.0f, 2833.0f, 3163.0f, 3571.0f,
};
static const size_t AP_LEN[4] = {211, 337, 449, 601};
#define AP_G 0.62f

#define MAX_SIZE 2.5f

#define TAIL_SIZE_MIN 0.7f
#define TAIL_SIZE_MAX 1.5f
#define TAIL_MOD_MIN 0.8f
#define TAIL_MOD_MAX 1.3f
#define TAIL_MIN_S 0.2f
#define TAIL_MAX_S 21.0f

#define CHAMBER_DC_HZ 12.0f
#define MAX_PREDELAY_MS 250.0f

void hadamard8(float s[CHAMBER_N]) {
    size_t step = 1;
    while (step < CHAMBER_N) {
        size_t i = 0;
        while (i < CHAMBER_N) {
            for (size_t j = i; j < i + step; j++) {
                float a = s[j];
                float b = s[j + step];
                s[j] = a + b;
                s[j + step] = a - b;
            }
            i += step * 2;
        }
        step *= 2;
    }
    float norm = 1.0f / sqrtf((float)CHAMBER_N);
    for (size_t i = 0; i < CHAMBER_N; i++) {
        s[i] *= norm;
    }
}

void chamber_init(Chamber *c, float sr) {
    sr = fmaxf(sr, 1.0f);
    float scale = sr / 48000.0f;
    size_t max_len = (size_t)(LEN[CHAMBER_N - 1] * MAX_SIZE * scale) + 64;
    c->sr = sr;
    delay_init(&c->pre, (size_t)(MAX_PREDELAY_MS * 0.001f * sr) + 8);
    c->hpf_l = (Biquad){0};
    c->hpf_r = (Biquad){0};
    c->pre_samples = 0;
    for (int i = 0; i < 4; i++) {
        delay_init(&c->ap[i], (size_t)((float)AP_LEN[i] * scale) + 8);
    }
    for (int i = 0; i < CHAMBER_N; i++) {
        delay_init(&c->lines[i], max_len);
        c->lp[i] = 0.0f;
        c->dc_x1[i] = 0.0f;
        c->dc_y1[i] = 0.0f;
        c->mod_ph[i] = (float)i * 0.7853f;
        c->len[i] = 0.0f;
        c->g[i] = 0.0f;
        c->len_to[i] = 0.0f;
        c->g_to[i] = 0.0f;
    }
    c->dc_r = expf(-TAU_F * CHAMBER_DC_HZ / sr);
    c->glide = expf(-1.0f / (0.04f * sr));
    c->damp_a = 0.0f;
    c->mod_samples = 0.0f;
    c->mix = 0.0f;
    c->mix_to = 0.0f;
    chamber_set(c, 0.0f, 0.0f);
}

void chamber_free(Chamber *c) {
    delay_free(&c->pre);
    for (int i = 0; i < 4; i++) {
        delay_free(&c->ap[i]);
    }
    for (int i = 0; i < CHAMBER_N; i++) {
        delay_free(&c->lines[i]);
    }
}

float chamber_tail_seconds(float tail) {
    tail = clampf(tail, 0.0f, 1.0f);
    return TAIL_MIN_S * powf(TAIL_MAX_S / TAIL_MIN_S, tail);
}

void chamber_set(Chamber *c, float dimension, float tail) {
    c->mix_to = clampf(dimension, 0.0f, 1.0f);
    tail = clampf(tail, 0.0f, 1.0f);
    float decay = TAIL_MIN_S * powf(TAIL_MAX_S / TAIL_MIN_S, tail);
    float size = TAIL_SIZE_MIN + (TAIL_SIZE_MAX - TAIL_SIZE_MIN) * tail;
    float scale = c->sr / 48000.0f * size;
    bool first = c->len[0] == 0.0f;
    for (int i = 0; i < CHAMBER_N; i++) {
        c->len_to[i] = fmaxf(LEN[i] * scale, 4.0f);
        float t = c->len_to[i] / c->sr;
        c->g_to[i] = fminf(powf(10.0f, -3.0f * t / decay), 0.9999f);
        if (first) {
            c->len[i] = c->len_to[i];
            c->g[i] = c->g_to[i];
        }
    }
    float corner = 18000.0f * powf(1.0f - CHAMBER_DAMP, 2.2f) + 700.0f * CHAMBER_DAMP;
    c->damp_a = clampf(expf(-TAU_F * corner / c->sr), 0.0f, 0.999f);
    c->pre_samples =
        (size_t)(clampf(CHAMBER_PREDELAY_MS, 0.0f, MAX_PREDELAY_MS) * 0.001f * c->sr);
    biquad_highpass(&c->hpf_l, CHAMBER_HPF_HZ, c->sr);
    biquad_highpass(&c->hpf_r, CHAMBER_HPF_HZ, c->sr);
    float mean_rate = 0.15f + 0.025f * ((float)CHAMBER_N - 1.0f) * 0.5f;
    float mod_scale = TAIL_MOD_MIN + (TAIL_MOD_MAX - TAIL_MOD_MIN) * tail;
    float ratio = powf(2.0f, CHAMBER_MOD_CENTS * mod_scale / 1200.0f) - 1.0f;
    c->mod_samples = ratio * c->sr / (TAU_F * mean_rate);
}

void chamber_clear(Chamber *c) {
    delay_clear(&c->pre);
    biquad_clear(&c->hpf_l);
    biquad_clear(&c->hpf_r);
    for (int i = 0; i < 4; i++) {
        delay_clear(&c->ap[i]);
    }
    for (int i = 0; i < CHAMBER_N; i++) {
        delay_clear(&c->lines[i]);
        c->lp[i] = 0.0f;
        c->dc_x1[i] = 0.0f;
        c->dc_y1[i] = 0.0f;
    }
}

Stereo chamber_process(Chamber *c, Stereo dry) {
    if (c->mix_to <= 0.0f && c->mix == 0.0f) {
        return dry;
    }
    float mono = 0.5f * (biquad_process(&c->hpf_l, dry.l) + biquad_process(&c->hpf_r, dry.r));
    delay_write(&c->pre, mono);
    float x = delay_read(&c->pre, c->pre_samples);
    for (int k = 0; k < 4; k++) {
        size_t l = (size_t)((float)AP_LEN[k] * (c->sr / 48000.0f));
        float v = delay_read(&c->ap[k], l);
        float y = -AP_G * x + v;
        delay_write(&c->ap[k], x + AP_G * y);
        x = y;
    }
    float s[CHAMBER_N];
    for (int i = 0; i < CHAMBER_N; i++) {
        c->mod_ph[i] += TAU_F * (0.15f + 0.025f * (float)i) / c->sr;
        if (c->mod_ph[i] > TAU_F) {
            c->mod_ph[i] -= TAU_F;
        }
        c->len[i] += (c->len_to[i] - c->len[i]) * (1.0f - c->glide);
        c->g[i] += (c->g_to[i] - c->g[i]) * (1.0f - c->glide);
        float wob = sinf(c->mod_ph[i]) * c->mod_samples;
        float v = delay_read_frac(&c->lines[i], fmaxf(c->len[i] + wob, 2.0f));
        c->lp[i] = v * (1.0f - c->damp_a) + c->lp[i] * c->damp_a;
        float fb = c->lp[i] * c->g[i];
        float y = fb - c->dc_x1[i] + c->dc_r * c->dc_y1[i];
        c->dc_x1[i] = fb;
        c->dc_y1[i] = y;
        s[i] = y;
    }
    hadamard8(s);
    for (int i = 0; i < CHAMBER_N; i++) {
        /* same energy normalisation as the room's combs: a longer tail rings
           longer, not louder */
        delay_write(&c->lines[i], s[i] + x * sqrtf(1.0f - c->g[i] * c->g[i]));
    }
    float mid = 0.0f;
    for (int i = 0; i < CHAMBER_N; i++) {
        mid += s[i];
    }
    mid *= 1.0f / sqrtf((float)CHAMBER_N);
    float side = (s[0] + s[2] - s[5] - s[7]) * 0.29f;
    Stereo wet = {mid + CHAMBER_WIDTH * side, mid - CHAMBER_WIDTH * side};
    c->mix += (c->mix_to - c->mix) * (1.0f - c->glide);
    if (c->mix_to <= 0.0f && c->mix < 1e-4f) {
        c->mix = 0.0f;
    }
    float m = c->mix;
    return (Stereo){dry.l * (1.0f - m) + wet.l * m, dry.r * (1.0f - m) + wet.r * m};
}
