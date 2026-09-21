#include "dsp.h"
#include <stdlib.h>
#include <string.h>

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

/* ---- output limiter ---- */

/* Cubic Lagrange interpolation at 4x catches the inter-sample overshoots a
   sample meter misses. The one-ms delay means every sample is attenuated by
   the largest reconstructed peak in its future look-ahead window. */
static float interp(float a, float b, float c, float d, float t) {
    float w0 = -((t - 1.0f) * (t - 2.0f) * (t - 3.0f)) / 6.0f;
    float w1 =  (t * (t - 2.0f) * (t - 3.0f)) / 2.0f;
    float w2 = -(t * (t - 1.0f) * (t - 3.0f)) / 2.0f;
    float w3 =  (t * (t - 1.0f) * (t - 2.0f)) / 6.0f;
    return a * w0 + b * w1 + c * w2 + d * w3;
}

static float reconstructed_peak(Limiter *l, Stereo x) {
    l->history_l[0] = l->history_l[1];
    l->history_l[1] = l->history_l[2];
    l->history_l[2] = l->history_l[3];
    l->history_l[3] = x.l;
    l->history_r[0] = l->history_r[1];
    l->history_r[1] = l->history_r[2];
    l->history_r[2] = l->history_r[3];
    l->history_r[3] = x.r;
    float peak = fmaxf(fabsf(x.l), fabsf(x.r));
    for (int n = 1; n < 4; n++) {
        float t = 1.0f + 0.25f * (float)n;
        float a = interp(l->history_l[0], l->history_l[1], l->history_l[2],
                         l->history_l[3], t);
        float b = interp(l->history_r[0], l->history_r[1], l->history_r[2],
                         l->history_r[3], t);
        peak = fmaxf(peak, fmaxf(fabsf(a), fabsf(b)));
    }
    return peak;
}

void limiter_init(Limiter *l, float sample_rate) {
    memset(l, 0, sizeof *l);
    l->len = (size_t)ceilf(fmaxf(sample_rate, 1.0f) * LIMITER_LOOKAHEAD_S) + 4;
    l->delay = calloc(l->len, sizeof *l->delay);
    l->peaks = calloc(l->len, sizeof *l->peaks);
    l->release_k = expf(-1.0f / (LIMITER_RELEASE_S * fmaxf(sample_rate, 1.0f)));
    limiter_set(l, true, LIMITER_CEILING_DB_DEFAULT);
    limiter_clear(l);
}

void limiter_free(Limiter *l) {
    free(l->delay);
    free(l->peaks);
    memset(l, 0, sizeof *l);
}

void limiter_clear(Limiter *l) {
    if (l->delay) memset(l->delay, 0, l->len * sizeof *l->delay);
    if (l->peaks) memset(l->peaks, 0, l->len * sizeof *l->peaks);
    memset(l->history_l, 0, sizeof l->history_l);
    memset(l->history_r, 0, sizeof l->history_r);
    l->write = 0;
    l->gain = 1.0f;
    l->reduction_db = 0.0f;
}

void limiter_set(Limiter *l, bool enabled, float ceiling_db) {
    l->enabled = enabled;
    ceiling_db = clampf(ceiling_db, LIMITER_CEILING_DB_MIN, LIMITER_CEILING_DB_MAX);
    l->ceiling = powf(10.0f, ceiling_db / 20.0f);
}

Stereo limiter_process(Limiter *l, Stereo x) {
    if (!l->delay || !l->peaks || l->len == 0) return x;
    float peak = reconstructed_peak(l, x);
    l->delay[l->write] = x;
    l->peaks[l->write] = peak;
    size_t read = (l->write + 1) % l->len;
    float window_peak = 0.0f;
    for (size_t i = 0; i < l->len; i++) window_peak = fmaxf(window_peak, l->peaks[i]);
    float target = l->enabled && window_peak > l->ceiling
        ? l->ceiling / window_peak : 1.0f;
    if (target < l->gain) l->gain = target;
    else l->gain = target + (l->gain - target) * l->release_k;
    Stereo y = {l->delay[read].l * l->gain, l->delay[read].r * l->gain};
    /* Guard float round-off and malformed input; it should never engage for
       valid limiter state, but preserves the output contract regardless. */
    if (l->enabled) {
        y.l = clampf(y.l, -l->ceiling, l->ceiling);
        y.r = clampf(y.r, -l->ceiling, l->ceiling);
    }
    l->reduction_db = -20.0f * log10f(fmaxf(l->gain, 1e-9f));
    l->write = read;
    return y;
}

float limiter_reduction_db(const Limiter *l) { return l->reduction_db; }

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
