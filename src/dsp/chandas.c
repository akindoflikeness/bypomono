#include "dsp.h"
#include <math.h>
#include <stdlib.h>

const Division CHANDAS_DIVISIONS[CHANDAS_DIVISIONS_LEN] = {
    {"8/1", 32.0f},
    {"4/1.", 24.0f},
    {"8/1T", 64.0f / 3.0f},
    {"4/1", 16.0f},
    {"2/1.", 12.0f},
    {"4/1T", 32.0f / 3.0f},
    {"2/1", 8.0f},
    {"1/1.", 6.0f},
    {"2/1T", 16.0f / 3.0f},
    {"1/1", 4.0f},
    {"1/2.", 3.0f},
    {"1/1T", 8.0f / 3.0f},
    {"1/2", 2.0f},
    {"1/4.", 1.5f},
    {"1/2T", 4.0f / 3.0f},
    {"1/4", 1.0f},
    {"1/8.", 0.75f},
    {"1/4T", 2.0f / 3.0f},
    {"1/8", 0.5f},
    {"1/16.", 0.375f},
    {"1/8T", 1.0f / 3.0f},
    {"1/16", 0.25f},
    {"1/32.", 0.1875f},
    {"1/16T", 1.0f / 6.0f},
    {"1/32", 0.125f},
};

const float chandas_subdivisions[CHANDAS_STREAMS] = {1.0f, 2.0f, 3.0f};

#define SPRAY_PERIODS 1.0f
#define REGEN 0.45f
/* note gaps outside this range are chords or pauses, not a pulse */
#define PULSE_MIN_SECONDS 0.06f
#define PULSE_MAX_SECONDS 4.0f
/* a reset zeroes the history over this long rather than in one sample */
#define CLEAR_SECONDS 0.05f

#define CHORUS_CENTRE_SECONDS 0.006f
#define CHORUS_SWEEP_SECONDS 0.005f
#define CHORUS_BUFFER_SECONDS 0.014f
#define CHORUS_RATE_HZ 0.55f
#define CHORUS_DEPTH_GLIDE_SECONDS 0.03f

ChandasParams chandas_params_default(void) {
    ChandasParams p;
    p.enabled = false;
    p.mix = 0.35f;
    p.sync = true;
    p.division = CHANDAS_DEFAULT_DIVISION;
    p.rate_hz = 1.0f;
    p.spread = 0.35f;
    p.size = 0.75f;
    p.warp = 0.0f;
    p.dimension = 0.0f;
    p.tail = 0.0f;
    return p;
}

float chandas_base_seconds_of(const ChandasParams *p, float bpm) {
    float raw;
    if (p->sync) {
        size_t d = p->division < CHANDAS_DIVISIONS_LEN - 1
                       ? p->division
                       : CHANDAS_DIVISIONS_LEN - 1;
        float beats = CHANDAS_DIVISIONS[d].beats;
        raw = beats * 60.0f / clampf(bpm, CHANDAS_MIN_BPM, CHANDAS_MAX_BPM);
    } else {
        raw = 1.0f / fmaxf(p->rate_hz, 0.01f);
    }
    return clampf(raw, 0.005f, CHANDAS_BUFFER_SECONDS);
}

bool chandas_is_capped(const ChandasParams *p, float bpm) {
    size_t d = p->division < CHANDAS_DIVISIONS_LEN - 1
                   ? p->division
                   : CHANDAS_DIVISIONS_LEN - 1;
    float beats = CHANDAS_DIVISIONS[d].beats;
    return p->sync &&
           beats * 60.0f / clampf(bpm, CHANDAS_MIN_BPM, CHANDAS_MAX_BPM) >
               CHANDAS_BUFFER_SECONDS;
}

float chandas_max_life(float delay, float rate, bool reverse, float buffer) {
    float room;
    if (reverse) {
        room = (buffer - delay) / (1.0f + rate);
    } else if (rate > 1.0f) {
        room = delay / (rate - 1.0f);
    } else if (rate < 1.0f) {
        room = (buffer - delay) / (1.0f - rate);
    } else {
        room = buffer;
    }
    return fmaxf(room * 0.9f, 8.0f);
}

float chandas_dice(uint32_t n) {
    uint32_t x = (n * 0x9E3779B9u) ^ 0x85EBCA6Bu;
    x ^= x >> 15;
    x *= 0x2545F491u;
    x ^= x >> 13;
    return (float)(x >> 8) / 16777216.0f;
}

static float buf_read(const float *buf, size_t n, float pos) {
    /* the cast below is only defined for a finite pos inside [0, n) */
    if (!(pos >= 0.0f && pos < (float)n)) {
        if (!isfinite(pos)) return 0.0f;
        pos -= (float)n * floorf(pos / (float)n);
        if (!(pos >= 0.0f && pos < (float)n)) return 0.0f;
    }
    float i = floorf(pos);
    float frac = pos - i;
    size_t a = (size_t)i % n;
    size_t b = (a + 1) % n;
    return buf[a] + (buf[b] - buf[a]) * frac;
}

static void chorus_init(Chorus *c, float sample_rate) {
    size_t n = (size_t)ceilf(CHORUS_BUFFER_SECONDS * sample_rate) + 4;
    c->buf_l = (float *)calloc(n, sizeof(float));
    c->buf_r = (float *)calloc(n, sizeof(float));
    c->len = n;
    c->w = 0;
    c->phase_l = 0.0f;
    c->phase_r = 1.0f / PHI;
    c->depth = 0.0f;
    c->sample_rate = sample_rate;
}

static void chorus_free(Chorus *c) {
    free(c->buf_l);
    free(c->buf_r);
    c->buf_l = NULL;
    c->buf_r = NULL;
}

static void chorus_clear(Chorus *c) {
    for (size_t i = 0; i < c->len; i++) {
        c->buf_l[i] = 0.0f;
        c->buf_r[i] = 0.0f;
    }
    c->w = 0;
    c->phase_l = 0.0f;
    c->phase_r = 1.0f / PHI;
    c->depth = 0.0f;
}

static Stereo chorus_process(Chorus *c, Stereo wet, float warp) {
    size_t n = c->len;
    c->buf_l[c->w] = flush_tiny(wet.l);
    c->buf_r[c->w] = flush_tiny(wet.r);
    c->w = (c->w + 1) % n;
    float k = 1.0f / fmaxf(CHORUS_DEPTH_GLIDE_SECONDS * c->sample_rate, 1.0f);
    c->depth += (clampf(warp, 0.0f, 1.0f) - c->depth) * k;
    if (c->depth <= 1e-5f) {
        c->depth = 0.0f;
        return wet;
    }
    float depth = c->depth;
    float rate_l = CHORUS_RATE_HZ / c->sample_rate;
    c->phase_l = fract_pos(c->phase_l + rate_l);
    c->phase_r = fract_pos(c->phase_r + rate_l / PHI);
    float sweep = CHORUS_SWEEP_SECONDS * c->sample_rate * depth;
    float centre = CHORUS_CENTRE_SECONDS * c->sample_rate;
    float nf = (float)n;
    float off_l = centre + sweep * 0.5f * (1.0f - cosf(TAU_F * c->phase_l));
    float off_r = centre + sweep * 0.5f * (1.0f - cosf(TAU_F * c->phase_r));
    Stereo out;
    out.l = buf_read(c->buf_l, n, fmodf((float)c->w - off_l + nf, nf));
    out.r = buf_read(c->buf_r, n, fmodf((float)c->w - off_r + nf, nf));
    return out;
}

static void chandas_place(Chandas *h) {
    for (size_t k = 0; k < CHANDAS_STREAMS; k++) {
        float even = (float)k / (float)(CHANDAS_STREAMS - 1);
        h->tap_pan[k] = 0.5f + (even - 0.5f) * CHANDAS_TAP_SPREAD;
    }
}

void chandas_init(Chandas *h, float sample_rate) {
    size_t n = (size_t)ceilf(CHANDAS_BUFFER_SECONDS * sample_rate) + 4;
    h->params = chandas_params_default();
    h->transport_running = true;
    h->sample_rate = sample_rate;
    h->bpm = CHANDAS_DEFAULT_BPM;
    h->buf_l = (float *)calloc(n, sizeof(float));
    h->buf_r = (float *)calloc(n, sizeof(float));
    h->len = n;
    h->w = 0;
    for (size_t i = 0; i < CHANDAS_MAX_GRAINS; i++) {
        Grain g = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false};
        h->grains[i] = g;
    }
    for (size_t k = 0; k < CHANDAS_STREAMS; k++) {
        h->slot[k] = 0;
        h->countdown[k] = 0.0f;
        h->tap_pan[k] = 0.5f;
    }
    chorus_init(&h->chorus, sample_rate);
    chamber_init(&h->chamber, sample_rate);
    h->mix_s = 0.0f;
    h->glide = expf(-1.0f / (0.04f * fmaxf(sample_rate, 1.0f)));
    dc_block_clear(&h->dc_loop_l);
    dc_block_clear(&h->dc_loop_r);
    dc_block_clear(&h->dc_out_l);
    dc_block_clear(&h->dc_out_r);
    h->spawned = 0;
    h->fade = 0.0f;
    h->clear_at = 0;
    h->clear_left = 0;
    h->fade_len = fmaxf(CHANDAS_RESET_FADE_SECONDS * sample_rate, 1.0f);
    h->harmony_interval = 0.0f;
    h->since_pulse = 0.0f;
    chandas_place(h);
}

void chandas_free(Chandas *h) {
    free(h->buf_l);
    free(h->buf_r);
    h->buf_l = NULL;
    h->buf_r = NULL;
    chorus_free(&h->chorus);
    chamber_free(&h->chamber);
}

ChandasParams chandas_params(const Chandas *h) { return h->params; }

void chandas_set_params(Chandas *h, ChandasParams p) {
    h->params = p;
    chamber_set(&h->chamber, p.dimension, p.tail);
}

void chandas_set_transport(Chandas *h, bool running) {
    h->transport_running = running;
}

void chandas_set_tempo(Chandas *h, float bpm) {
    h->bpm = clampf(bpm, CHANDAS_MIN_BPM, CHANDAS_MAX_BPM);
}

void chandas_note_pulse(Chandas *h) {
    float gap = h->since_pulse / fmaxf(h->sample_rate, 1.0f);
    if (gap >= PULSE_MIN_SECONDS && gap <= PULSE_MAX_SECONDS) {
        h->harmony_interval = gap;
    }
    h->since_pulse = 0.0f;
}

float chandas_tempo(const Chandas *h) { return h->bpm; }

void chandas_reset(Chandas *h) {
    if (h->fade <= 0.0f) {
        h->fade = h->fade_len;
    }
}

/* zeroes the next stretch of old history. The cursor starts just ahead of
   the write head and runs faster than it, so it never erases a fresh write. */
static void chandas_clear_step(Chandas *h) {
    size_t n = (size_t)ceilf((float)h->len / (CLEAR_SECONDS * h->sample_rate));
    if (n > h->clear_left) n = h->clear_left;
    for (size_t i = 0; i < n; i++) {
        h->buf_l[h->clear_at] = 0.0f;
        h->buf_r[h->clear_at] = 0.0f;
        h->clear_at = (h->clear_at + 1) % h->len;
    }
    h->clear_left -= n;
}

static void chandas_clear(Chandas *h) {
    h->clear_at = (h->w + 1) % h->len;
    h->clear_left = h->len - 1;
    for (size_t i = 0; i < CHANDAS_MAX_GRAINS; i++) {
        Grain g = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false};
        h->grains[i] = g;
    }
    for (size_t k = 0; k < CHANDAS_STREAMS; k++) {
        h->slot[k] = 0;
        h->countdown[k] = 0.0f;
    }
    h->spawned = 0;
    chorus_clear(&h->chorus);
    chamber_clear(&h->chamber);
    dc_block_clear(&h->dc_loop_l);
    dc_block_clear(&h->dc_loop_r);
    dc_block_clear(&h->dc_out_l);
    dc_block_clear(&h->dc_out_r);
}

float chandas_base_seconds(const Chandas *h) {
    if (h->params.sync && h->harmony_interval > 0.0f) {
        size_t d = h->params.division < CHANDAS_DIVISIONS_LEN - 1
                       ? h->params.division
                       : CHANDAS_DIVISIONS_LEN - 1;
        float beats = CHANDAS_DIVISIONS[d].beats;
        return clampf(beats * h->harmony_interval, 0.005f,
                      CHANDAS_BUFFER_SECONDS);
    }
    return chandas_base_seconds_of(&h->params, h->bpm);
}

static void chandas_spawn(Chandas *h, size_t k, float entry) {
    float n = (float)h->len;
    float base = chandas_base_seconds(h) * h->sample_rate;
    float rate = 1.0f;
    h->spawned += 1;
    float warp = clampf(h->params.warp, 0.0f, 1.0f);
    bool reverse = chandas_dice(h->spawned) < warp;
    float spray = chandas_dice(h->spawned * 2654435761u);
    float scatter =
        SPRAY_PERIODS * base * clampf(h->params.spread, 0.0f, 1.0f) * spray;
    float delay = clampf(entry + scatter, 8.0f, n * 0.9f);
    float want = entry * clampf(h->params.size, CHANDAS_MIN_SIZE, CHANDAS_MAX_SIZE);
    float life =
        fmaxf(fminf(want, chandas_max_life(delay, rate, reverse, n)), 8.0f);
    float pos = fmodf((float)h->w - delay + n, n);
    float theta = h->tap_pan[k] * (0.5f * PI_F);
    size_t s = k * 2 + (size_t)h->slot[k];
    h->slot[k] ^= 1;
    h->grains[s].pos = pos;
    h->grains[s].step = reverse ? -rate : rate;
    h->grains[s].age = 0.0f;
    h->grains[s].life = life;
    h->grains[s].gain_l = cosf(theta);
    h->grains[s].gain_r = sinf(theta);
    h->grains[s].active = true;
}

static void chandas_write(Chandas *h, Stereo dry, Stereo wet) {
    float fl = dc_block_process(&h->dc_loop_l, wet.l);
    float fr = dc_block_process(&h->dc_loop_r, wet.r);
    h->buf_l[h->w] = flush_tiny(dry.l + fl * REGEN);
    h->buf_r[h->w] = flush_tiny(dry.r + fr * REGEN);
    h->w = (h->w + 1) % h->len;
}

Stereo chandas_process(Chandas *h, Stereo dry) {
    h->since_pulse += 1.0f;
    /* switched off, the grains keep sounding until the mix has travelled to
       nothing: returning the dry signal outright drops the wet in one sample */
    if (!h->params.enabled && h->mix_s <= 0.0f) {
        Stereo zero = {0.0f, 0.0f};
        if (h->clear_left > 0) chandas_clear_step(h);
        chandas_write(h, dry, zero);
        h->mix_s += (0.0f - h->mix_s) * (1.0f - h->glide);
        if (h->mix_s < 1e-4f) {
            h->mix_s = 0.0f;
        }
        if (h->fade > 0.0f) {
            h->fade -= 1.0f;
            if (h->fade <= 0.0f) {
                chandas_clear(h);
            }
        }
        return dry;
    }
    bool clearing = h->clear_left > 0;
    if (clearing) chandas_clear_step(h);
    float base = chandas_base_seconds(h) * h->sample_rate;
    for (size_t k = 0; k < CHANDAS_STREAMS; k++) {
        float entry = fmaxf(base / chandas_subdivisions[k], 8.0f);
        /* a shorter period takes over now, not after the old one runs out */
        if (h->countdown[k] > entry) h->countdown[k] = entry;
        if (h->countdown[k] <= 0.0f) {
            /* no new grains once it is switched off; the ones in the air finish.
               None while old history is still being zeroed either. */
            if (h->params.enabled && h->transport_running && !clearing)
                chandas_spawn(h, k, entry);
            h->countdown[k] += entry;
        }
        h->countdown[k] -= 1.0f;
    }
    float wl = 0.0f, wr = 0.0f;
    float n = (float)h->len;
    for (size_t i = 0; i < CHANDAS_MAX_GRAINS; i++) {
        Grain *g = &h->grains[i];
        if (!g->active) {
            continue;
        }
        float win = 0.5f - 0.5f * cosf(TAU_F * (g->age / g->life));
        wl += buf_read(h->buf_l, h->len, g->pos) * win * g->gain_l;
        wr += buf_read(h->buf_r, h->len, g->pos) * win * g->gain_r;
        g->pos += g->step;
        if (g->pos >= n) {
            g->pos -= n;
        } else if (g->pos < 0.0f) {
            g->pos += n;
        }
        g->age += 1.0f;
        if (g->age >= g->life) {
            g->active = false;
        }
    }
    float norm = 1.0f / sqrtf((float)CHANDAS_STREAMS);
    Stereo pre = {wl * norm, wr * norm};
    Stereo wet = chorus_process(&h->chorus, pre, h->params.warp);
    chandas_write(h, dry, wet);
    wet.l = dc_block_process(&h->dc_out_l, wet.l);
    wet.r = dc_block_process(&h->dc_out_r, wet.r);
    wet = chamber_process(&h->chamber, wet);
    if (h->fade > 0.0f) {
        h->fade -= 1.0f;
        float g = 0.5f - 0.5f * cosf(PI_F * (h->fade / h->fade_len));
        if (h->fade <= 0.0f) {
            chandas_clear(h);
        }
        wet.l *= g;
        wet.r *= g;
    }
    float target = h->params.enabled ? clampf(h->params.mix, 0.0f, 1.0f) : 0.0f;
    h->mix_s += (target - h->mix_s) * (1.0f - h->glide);
    if (target <= 0.0f && h->mix_s < 1e-4f) {
        h->mix_s = 0.0f;
    }
    float mix = h->mix_s;
    Stereo out = {dry.l * (1.0f - mix) + wet.l * mix,
                  dry.r * (1.0f - mix) + wet.r * mix};
    return out;
}
