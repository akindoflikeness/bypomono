#ifndef BYPO_TEST_CLICK_H
#define BYPO_TEST_CLICK_H

/* A C port of ~/dev/seam, the detector written for the Rust build and kept
   outside it because none of it is about this instrument.
   
   The method is a counterfactual. Render the same deterministic probe twice
   from the same constructor: once with the change applied, once with it never
   applied. Until the change has an effect the two runs are bit-identical, so
   the first sample that differs is the event, exactly, with no threshold to
   argue about — and subtracting one run from the other leaves only what the
   change did. That is what makes this readable on an instrument whose own
   signal is near-Nyquist: the material cancels.
   
   Reported in dB relative to the signal's RMS, so the numbers mean the same
   thing at any volume. Around -20 dB is obvious, -40 is audible on
   headphones in a quiet passage, -60 is not there.
   
   A measurement that could not be made is not a pass. */

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* The level a seam has to stay under, measured against seam's own probe. */
#define SEAM_AUDIBLE_DB (-40.0f)

typedef struct {
    size_t warmup;  /* rendered and discarded, so the change lands on a settled instrument */
    size_t pre;     /* sets the signal level and the motion the seam is judged against */
    size_t post;    /* rendered after; only `horizon` samples of it are measured */
    size_t horizon; /* longer than any click, shorter than any gesture */
    size_t block;   /* a probe that only behaves at its host's block size is caught here */
} SeamConfig;

static inline SeamConfig seam_defaults(float sample_rate) {
    SeamConfig c;
    c.warmup = (size_t)(sample_rate * 0.25f);
    c.pre = (size_t)(sample_rate * 0.09f);
    c.post = (size_t)(sample_rate * 0.05f);
    c.horizon = (size_t)(sample_rate * 0.0005f); /* half a millisecond */
    c.block = 64;
    return c;
}

typedef enum {
    SEAM_MEASURED,
    SEAM_SILENT,           /* nothing was sounding, so there was no seam */
    SEAM_NONDETERMINISTIC, /* the runs differ before the change: no counterfactual */
    SEAM_TOO_SHORT
} SeamKind;

typedef struct {
    SeamKind kind;
    float seam_db;   /* how abruptly the change arrived */
    float motion_db; /* what the sound was already doing: the masking reference */
    float excess_db; /* how far it stuck out of the material it landed in */
    bool moved;
    size_t deferred_by; /* samples between the control moving and the sound moving */
} Seam;

/* make: build a fresh probe. render: fill n samples. apply: make the change. */
typedef struct {
    void *ctx;
    void (*make)(void *ctx);
    void (*render)(void *ctx, float *out, size_t n);
    void (*apply)(void *ctx);
    void (*teardown)(void *ctx);
} SeamProbe;

static inline void seam_in_blocks(SeamProbe *p, float *out, size_t n, size_t block) {
    if (block < 1) block = 1;
    for (size_t at = 0; at < n; at += block) {
        size_t run = block < n - at ? block : n - at;
        p->render(p->ctx, out + at, run);
    }
}

/* one run: warm up, render, apply between two calls, render */
static inline void seam_run(SeamConfig cfg, SeamProbe *p, bool apply, float *out) {
    p->make(p->ctx);
    float *scratch = (float *)calloc(cfg.warmup, sizeof(float));
    seam_in_blocks(p, scratch, cfg.warmup, cfg.block);
    free(scratch);
    seam_in_blocks(p, out, cfg.pre, cfg.block);
    if (apply) p->apply(p->ctx);
    seam_in_blocks(p, out + cfg.pre, cfg.post, cfg.block);
    p->teardown(p->ctx);
}

static inline Seam seam_check(SeamConfig cfg, SeamProbe probe) {
    Seam s;
    memset(&s, 0, sizeof s);
    if (cfg.pre < 2 || cfg.post < cfg.horizon || cfg.horizon == 0) {
        s.kind = SEAM_TOO_SHORT;
        return s;
    }
    size_t len = cfg.pre + cfg.post;
    float *changed = (float *)calloc(len, sizeof(float));
    float *held = (float *)calloc(len, sizeof(float));
    seam_run(cfg, &probe, true, changed);
    seam_run(cfg, &probe, false, held);

    if (memcmp(changed, held, cfg.pre * sizeof(float)) != 0) {
        s.kind = SEAM_NONDETERMINISTIC;
        free(changed);
        free(held);
        return s;
    }
    double sum = 0.0, motion = 0.0;
    for (size_t i = 0; i < cfg.pre; i++) sum += (double)held[i] * held[i];
    for (size_t i = 1; i < cfg.pre; i++) {
        double d = (double)held[i] - held[i - 1];
        motion += d * d;
    }
    float rms = (float)sqrt(sum / (double)cfg.pre);
    if (rms < 1e-5f) {
        s.kind = SEAM_SILENT;
        free(changed);
        free(held);
        return s;
    }
    float move = (float)sqrt(motion / (double)(cfg.pre - 1));

    /* the runs are bit-identical until the change lands, so the first
       difference is the event */
    size_t onset = 0;
    bool found = false;
    for (size_t i = cfg.pre; i < len; i++) {
        if (changed[i] != held[i]) {
            onset = i;
            found = true;
            break;
        }
    }
    float abrupt = 0.0f;
    if (found) {
        size_t reach = cfg.horizon < len - onset ? cfg.horizon : len - onset;
        for (size_t j = 0; j < reach; j++) {
            float d = fabsf(changed[onset + j] - held[onset + j]);
            float rate = d / (float)(j + 1);
            if (rate > abrupt) abrupt = rate;
        }
    }
    s.kind = SEAM_MEASURED;
    s.moved = found;
    s.deferred_by = found ? onset - cfg.pre : 0;
    s.seam_db = 20.0f * log10f(fmaxf(abrupt / rms, 1e-20f));
    s.motion_db = 20.0f * log10f(fmaxf(move / rms, 1e-20f));
    s.excess_db = s.seam_db - s.motion_db;
    free(changed);
    free(held);
    return s;
}

static inline const char *seam_why(SeamKind k) {
    switch (k) {
    case SEAM_MEASURED: return "measured";
    case SEAM_SILENT: return "nothing was sounding, so there was no seam to measure";
    case SEAM_NONDETERMINISTIC: return "the runs differ before the change, so there is no counterfactual";
    case SEAM_TOO_SHORT: return "the windows leave nothing to measure";
    }
    return "?";
}

#endif
