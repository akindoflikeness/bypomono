#include "../src/dsp/dsp.h"
#include "test.h"
#include <math.h>
#include <stdlib.h>

static Stereo *drive(Chamber *c, float secs, float sr, size_t *out_n) {
    size_t n = (size_t)(sr * secs);
    Stereo *out = malloc(n * sizeof(Stereo));
    for (size_t i = 0; i < n; i++) {
        float x = sinf((float)i * 0.05f) * 0.5f;
        out[i] = chamber_process(c, (Stereo){x, x});
    }
    *out_n = n;
    return out;
}

static double rms(const Stereo *s, size_t len) {
    double n = (double)(len > 1 ? len : 1);
    double sum = 0.0;
    for (size_t i = 0; i < len; i++) {
        sum += (double)(s[i].l * s[i].l + s[i].r * s[i].r);
    }
    return sqrt(sum / n);
}

static void dimension_zero_is_bypassed_bit_identically(void) {
    Chamber c;
    chamber_init(&c, 48000.0f);
    chamber_set(&c, 0.0f, 1.0f);
    for (int i = 0; i < 8192; i++) {
        float x = sinf((float)i * 0.05f) * 0.5f;
        Stereo y = chamber_process(&c, (Stereo){x, -x});
        CHECK(y.l == x && y.r == -x, "sample %d", i);
    }
    chamber_free(&c);
}

static void it_adds_no_offset_to_a_signal_that_had_none(void) {
    static const float cases[3][2] = {{1.0f, 1.0f}, {1.0f, 0.5f}, {1.0f, 0.0f}};
    for (int k = 0; k < 3; k++) {
        float dim = cases[k][0];
        float tail = cases[k][1];
        Chamber c;
        chamber_init(&c, 48000.0f);
        chamber_set(&c, dim, tail);
        size_t n0, n;
        free(drive(&c, 4.0f, 48000.0f, &n0));
        Stereo *out = drive(&c, 2.0f, 48000.0f, &n);
        double mean = 0.0;
        for (size_t i = 0; i < n; i++) {
            mean += (double)(out[i].l + out[i].r) * 0.5;
        }
        mean /= (double)n;
        double level = rms(out, n);
        CHECK(fabs(mean) < 0.01 * level,
              "dimension %g, tail %g: offset %.5f against level %.4f", dim, tail, mean,
              level);
        free(out);
        chamber_free(&c);
    }
}

/* nothing in the loop clips, so a held tone can build a little on a mode;
   it must not keep growing */
static void it_stays_bounded_however_hard_it_is_driven(void) {
    Chamber c;
    chamber_init(&c, 48000.0f);
    chamber_set(&c, 1.0f, 1.0f);
    float peak = 0.0f;
    for (int i = 0; i < 48000 * 4; i++) {
        float x = sinf((float)i * 0.05f) * 8.0f;
        Stereo y = chamber_process(&c, (Stereo){x, x});
        CHECK(isfinite(y.l) && isfinite(y.r), "not finite at %d", i);
        if (i > 48000 / 4) {
            peak = fmaxf(fmaxf(peak, fabsf(y.l)), fabsf(y.r));
        }
    }
    CHECK(peak < 8.0f * 3.0f, "chamber ran away: peak %g from 8 in", peak);
    chamber_free(&c);
}

static void ring(float tail, double *head_out, double *end_out) {
    float sr = 48000.0f;
    Chamber c;
    chamber_init(&c, sr);
    chamber_set(&c, 1.0f, tail);
    size_t n0;
    free(drive(&c, 2.0f, sr, &n0));
    size_t qn = (size_t)(sr * 3.0f);
    Stereo *quiet = malloc(qn * sizeof(Stereo));
    for (size_t i = 0; i < qn; i++) {
        quiet[i] = chamber_process(&c, (Stereo){0.0f, 0.0f});
    }
    *head_out = rms(quiet, 4800);
    *end_out = rms(quiet + (qn - 4800), 4800);
    free(quiet);
    chamber_free(&c);
}

static void the_tail_decays_and_tail_sets_how_long(void) {
    double short_head, short_end, long_head, long_end;
    ring(0.05f, &short_head, &short_end);
    CHECK(short_end < short_head * 1e-3, "a short tail did not end");
    ring(1.0f, &long_head, &long_end);
    CHECK(long_end > short_end, "TAIL did not lengthen the decay");
}

static void dimension_glides_rather_than_clicking(void) {
    float sr = 48000.0f;
    Chamber c;
    chamber_init(&c, sr);
    chamber_set(&c, 0.1f, 0.6f);
    for (size_t i = 0; i < (size_t)(sr * 3.0f); i++) {
        float t = sinf((float)i * 0.02f) * 0.5f;
        chamber_process(&c, (Stereo){t, t});
    }
    chamber_set(&c, 1.0f, 0.6f);
    float t0 = sinf(0.0f * 0.02f) * 0.5f;
    float prev = chamber_process(&c, (Stereo){t0, t0}).l;
    float worst = 0.0f;
    for (int i = 1; i < 8192; i++) {
        float t = sinf((float)i * 0.02f) * 0.5f;
        float l = chamber_process(&c, (Stereo){t, t}).l;
        worst = fmaxf(worst, fabsf(l - prev));
        prev = l;
    }
    CHECK(worst < 0.1f, "DIMENSION clicked: worst single-sample jump %g", worst);
    chamber_free(&c);
}

static void mixer_render(float dim, Stereo *dry_out, Stereo *wet_out, size_t *out_n) {
    float sr = 48000.0f;
    Chamber c;
    chamber_init(&c, sr);
    chamber_set(&c, dim, 0.6f);
    size_t n = 0;
    for (size_t i = 0; i < (size_t)sr * 2; i++) {
        Stereo d = {sinf((float)i * 0.02f) * 0.5f, sinf((float)i * 0.017f) * 0.4f};
        Stereo y = chamber_process(&c, d);
        if (i >= (size_t)sr) {
            dry_out[n] = d;
            wet_out[n] = y;
            n++;
        }
    }
    *out_n = n;
    chamber_free(&c);
}

static void dimension_is_the_mixer_and_only_the_mixer(void) {
    size_t cap = 48000;
    Stereo *d0 = malloc(cap * sizeof(Stereo));
    Stereo *y0 = malloc(cap * sizeof(Stereo));
    Stereo *d1 = malloc(cap * sizeof(Stereo));
    Stereo *y1 = malloc(cap * sizeof(Stereo));
    Stereo *dh = malloc(cap * sizeof(Stereo));
    Stereo *yh = malloc(cap * sizeof(Stereo));
    size_t n0, n1, nh;
    mixer_render(0.0f, d0, y0, &n0);
    for (size_t i = 0; i < n0; i++) {
        CHECK(y0[i].l == d0[i].l && y0[i].r == d0[i].r,
              "DIMENSION 0 is not the dry signal untouched");
    }
    mixer_render(1.0f, d1, y1, &n1);
    mixer_render(0.5f, dh, yh, &nh);
    for (size_t k = 0; k < n1 && k < nh; k++) {
        float want_l = 0.5f * d1[k].l + 0.5f * y1[k].l;
        float want_r = 0.5f * d1[k].r + 0.5f * y1[k].r;
        CHECK(fabsf(yh[k].l - want_l) < 1e-4f && fabsf(yh[k].r - want_r) < 1e-4f,
              "sample %zu: half wet (%g, %g) is not half of full wet, wanted (%g, %g)",
              k, yh[k].l, yh[k].r, want_l, want_r);
    }
    free(d0);
    free(y0);
    free(d1);
    free(y1);
    free(dh);
    free(yh);
}

static Stereo *tail_render(float tail, size_t *out_n) {
    float sr = 48000.0f;
    Chamber c;
    chamber_init(&c, sr);
    chamber_set(&c, 1.0f, tail);
    Stereo *out = drive(&c, 1.0f, sr, out_n);
    chamber_free(&c);
    return out;
}

static void tail_moves_the_space_it_rings_in(void) {
    float sr = 48000.0f;
    size_t na, nb;
    Stereo *a = tail_render(0.0f, &na);
    Stereo *b = tail_render(1.0f, &nb);
    bool differs = na != nb;
    for (size_t i = 0; !differs && i < na; i++) {
        differs = a[i].l != b[i].l || a[i].r != b[i].r;
    }
    CHECK(differs, "TAIL did not move the space");
    free(a);
    free(b);
    size_t nn, nf;
    Stereo *near_ = tail_render(0.2f, &nn);
    Stereo *far_ = tail_render(0.8f, &nf);
    size_t lo = (size_t)(sr * 0.2f);
    size_t hi = (size_t)(sr * 0.35f);
    double early_near = rms(near_ + lo, hi - lo);
    double early_far = rms(far_ + lo, hi - lo);
    CHECK(fabs(early_near - early_far) > 1e-4,
          "TAIL changed only the decay, not the room");
    free(near_);
    free(far_);
}

static void the_hadamard_preserves_energy(void) {
    float s[CHAMBER_N] = {0.3f, -0.7f, 1.1f, 0.0f, -0.2f, 0.9f, 0.4f, -1.3f};
    float before = 0.0f;
    for (int i = 0; i < CHAMBER_N; i++) {
        before += s[i] * s[i];
    }
    hadamard8(s);
    float after = 0.0f;
    for (int i = 0; i < CHAMBER_N; i++) {
        after += s[i] * s[i];
    }
    CHECK(fabsf(before - after) < 1e-4f, "%g -> %g", before, after);
}

void test_chamber(void) {
    dimension_zero_is_bypassed_bit_identically();
    it_adds_no_offset_to_a_signal_that_had_none();
    it_stays_bounded_however_hard_it_is_driven();
    the_tail_decays_and_tail_sets_how_long();
    dimension_glides_rather_than_clicking();
    dimension_is_the_mixer_and_only_the_mixer();
    tail_moves_the_space_it_rings_in();
    the_hadamard_preserves_energy();
}
