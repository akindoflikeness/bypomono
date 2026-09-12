#include "dsp.h"

float powi_f(float base, int n) {
    unsigned e = n < 0 ? (unsigned)(-(long)n) : (unsigned)n;
    float acc = 1.0f, b = base;
    while (e) {
        if (e & 1u) acc *= b;
        b *= b;
        e >>= 1;
    }
    return n < 0 ? 1.0f / acc : acc;
}

/* ---- phase ---- */

float allpass_phase(float a, float w) {
    float s = sinf(w), c = cosf(w);
    float num = atan2f(-s, a + c);
    float den = atan2f(-a * s, 1.0f + a * c);
    return num - den;
}

float allpass_coeff_for(float hz, float sample_rate, float target) {
    float w = TAU_F * hz / sample_rate;
    float lo = -0.99f, hi = 0.999f;
    for (int i = 0; i < 48; i++) {
        float mid = 0.5f * (lo + hi);
        if (allpass_phase(mid, w) < -target)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5f * (lo + hi);
}

void phase_rotator_init(PhaseRotator *p, float a) {
    p->a = a;
    p->x1 = 0.0f;
    p->y1 = 0.0f;
}

float phase_rotator_process(PhaseRotator *p, float x) {
    float y = p->a * x + p->x1 - p->a * p->y1;
    p->x1 = x;
    p->y1 = y;
    return y;
}

void phase_rotator_clear(PhaseRotator *p) {
    p->x1 = 0.0f;
    p->y1 = 0.0f;
}

/* ---- master ---- */

float soft_clip(float x) {
    float a = fabsf(x);
    if (a < SOFT_CLIP_KNEE) return x;
    return copysignf(
        SOFT_CLIP_KNEE + (1.0f - SOFT_CLIP_KNEE)
                             * tanhf((a - SOFT_CLIP_KNEE) / (1.0f - SOFT_CLIP_KNEE)),
        x);
}

float soft_clip_to(float x, float ceiling) {
    return soft_clip(x / ceiling) * ceiling;
}

/* ---- gate ---- */

void engage_gate_init(EngageGate *g, float sample_rate, bool engaged) {
    g->gain = engaged ? 1.0f : 0.0f;
    g->k = 1.0f - expf(-1.0f / (GATE_GLIDE_S * fmaxf(sample_rate, 1.0f)));
}

float engage_gate_next(EngageGate *g, bool open) {
    float target = open ? 1.0f : 0.0f;
    g->gain += (target - g->gain) * g->k;
    if (!open && g->gain < GATE_FLOOR) g->gain = 0.0f;
    return g->gain;
}

/* ---- shared primitives ---- */

float dc_block_process(DcBlock *d, float x) {
    float y = x - d->x1 + DC_BLOCK_R * d->y1;
    d->x1 = x;
    d->y1 = y;
    return y;
}

void dc_block_clear(DcBlock *d) {
    d->x1 = 0.0f;
    d->y1 = 0.0f;
}

float svf_process(Svf *s, float x, float g, float k) {
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;
    float v3 = x - s->ic2;
    float v1 = a1 * s->ic1 + a2 * v3;
    float v2 = s->ic2 + a2 * s->ic1 + a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    return v2;
}

float svf_process_hp(Svf *s, float x, float g, float k) {
    float a1 = 1.0f / (1.0f + g * (g + k));
    float a2 = g * a1;
    float a3 = g * a2;
    float v3 = x - s->ic2;
    float v1 = a1 * s->ic1 + a2 * v3;
    float v2 = s->ic2 + a2 * s->ic1 + a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    return x - k * v1 - v2;
}

static void biquad_terms(float hz, float sr, float *cs, float *alpha) {
    const float q = 0.70710678f;
    float w0 = TAU_F * clampf(hz / fmaxf(sr, 1.0f), 1e-5f, 0.45f);
    float sn = sinf(w0);
    *cs = cosf(w0);
    *alpha = sn / (2.0f * q);
}

void biquad_highpass(Biquad *q, float hz, float sr) {
    float cs, alpha;
    biquad_terms(hz, sr, &cs, &alpha);
    float a0 = 1.0f + alpha;
    q->b0 = ((1.0f + cs) * 0.5f) / a0;
    q->b1 = (-(1.0f + cs)) / a0;
    q->b2 = q->b0;
    q->a1 = (-2.0f * cs) / a0;
    q->a2 = (1.0f - alpha) / a0;
}

void biquad_allpass(Biquad *q, float hz, float sr) {
    float cs, alpha;
    biquad_terms(hz, sr, &cs, &alpha);
    float a0 = 1.0f + alpha;
    q->b0 = (1.0f - alpha) / a0;
    q->b1 = (-2.0f * cs) / a0;
    q->b2 = 1.0f;
    q->a1 = (-2.0f * cs) / a0;
    q->a2 = (1.0f - alpha) / a0;
}

float biquad_process(Biquad *q, float x) {
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

void biquad_clear(Biquad *q) {
    q->z1 = 0.0f;
    q->z2 = 0.0f;
}

#include <stdlib.h>
#include <string.h>

void delay_init(Delay *d, size_t max) {
    d->len = max < 4 ? 4 : max;
    d->buf = calloc(d->len, sizeof(float));
    d->w = 0;
}

void delay_free(Delay *d) {
    free(d->buf);
    d->buf = NULL;
    d->len = 0;
}

void delay_write(Delay *d, float x) {
    d->buf[d->w] = x;
    d->w = (d->w + 1) % d->len;
}

float delay_read(const Delay *d, size_t back) {
    size_t n = d->len;
    size_t b = back < n - 1 ? back : n - 1;
    return d->buf[(d->w + n - b) % n];
}

float delay_read_frac(const Delay *d, float back) {
    size_t n = d->len;
    float dd = clampf(back, 1.0f, (float)(n - 2));
    float i = floorf(dd);
    float frac = dd - i;
    size_t a = (d->w + n - (size_t)i) % n;
    size_t b = (a + n - 1) % n;
    return d->buf[a] + (d->buf[b] - d->buf[a]) * frac;
}

void delay_clear(Delay *d) {
    memset(d->buf, 0, d->len * sizeof(float));
    d->w = 0;
}
