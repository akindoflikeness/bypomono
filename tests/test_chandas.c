#include "../src/dsp/dsp.h"
#include "test.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern const float chandas_subdivisions[CHANDAS_STREAMS];

#define SR 48000.0f

static void eng(Chandas *h, ChandasParams p) {
    chandas_init(h, SR);
    chandas_set_params(h, p);
}

static ChandasParams on_(float size, float spread) {
    ChandasParams p = chandas_params_default();
    p.enabled = true;
    p.mix = 1.0f;
    p.sync = false;
    p.rate_hz = 2.0f;
    p.size = size;
    p.spread = spread;
    return p;
}

static float division_by_name(const char *n) {
    for (size_t i = 0; i < CHANDAS_DIVISIONS_LEN; i++) {
        if (strcmp(CHANDAS_DIVISIONS[i].name, n) == 0) {
            return CHANDAS_DIVISIONS[i].beats;
        }
    }
    CHECK(false, "division %s not found", n);
    return 0.0f;
}

static void the_division_ladder_runs_from_8_1_down_to_1_32(void) {
    CHECK(strcmp(CHANDAS_DIVISIONS[0].name, "8/1") == 0, "first name");
    CHECK(strcmp(CHANDAS_DIVISIONS[CHANDAS_DIVISIONS_LEN - 1].name, "1/32") == 0,
          "last name");
    CHECK(CHANDAS_DIVISIONS[0].beats == 32.0f, "first beats");
    CHECK(CHANDAS_DIVISIONS[CHANDAS_DIVISIONS_LEN - 1].beats == 0.125f,
          "last beats");
    for (size_t i = 0; i + 1 < CHANDAS_DIVISIONS_LEN; i++) {
        CHECK(CHANDAS_DIVISIONS[i].beats > CHANDAS_DIVISIONS[i + 1].beats,
              "not monotonic: %s then %s", CHANDAS_DIVISIONS[i].name,
              CHANDAS_DIVISIONS[i + 1].name);
    }
    CHECK(fabsf(division_by_name("1/4.") - division_by_name("1/4") * 1.5f) <
              1e-6f,
          "dotted quarter");
    CHECK(fabsf(division_by_name("1/4T") -
                division_by_name("1/4") * 2.0f / 3.0f) < 1e-6f,
          "triplet quarter");
    CHECK(strcmp(CHANDAS_DIVISIONS[CHANDAS_DEFAULT_DIVISION].name, "1/2") == 0,
          "default division");
}

static void no_setting_can_outrun_the_buffer(void) {
    ChandasParams p = chandas_params_default();
    const float bpms[4] = {CHANDAS_MIN_BPM, 60.0f, CHANDAS_DEFAULT_BPM,
                           CHANDAS_MAX_BPM};
    for (int bi = 0; bi < 4; bi++) {
        float bpm = bpms[bi];
        for (size_t d = 0; d < CHANDAS_DIVISIONS_LEN; d++) {
            p.division = d;
            p.sync = true;
            float s = chandas_base_seconds_of(&p, bpm);
            CHECK(s > 0.0f && s <= CHANDAS_BUFFER_SECONDS, "%s at %g: %g",
                  CHANDAS_DIVISIONS[d].name, bpm, s);
        }
        p.sync = false;
        const float hzs[3] = {0.1f, 1.0f, 8.0f};
        for (int hi = 0; hi < 3; hi++) {
            p.rate_hz = hzs[hi];
            float s = chandas_base_seconds_of(&p, bpm);
            CHECK(s > 0.0f && s <= CHANDAS_BUFFER_SECONDS, "free %g hz: %g",
                  hzs[hi], s);
        }
    }
}

static void the_longest_rung_reports_when_it_is_capped(void) {
    ChandasParams p = chandas_params_default();
    p.sync = true;
    p.division = 0;
    CHECK(chandas_is_capped(&p, 40.0f), "not capped at 40");
    CHECK(chandas_base_seconds_of(&p, 40.0f) == CHANDAS_BUFFER_SECONDS,
          "not clamped to buffer");
    CHECK(!chandas_is_capped(&p, 120.0f), "capped at 120");
    CHECK(fabsf(chandas_base_seconds_of(&p, 120.0f) - 16.0f) < 1e-4f,
          "8/1 at 120: %g", chandas_base_seconds_of(&p, 120.0f));
    ChandasParams free_p = chandas_params_default();
    free_p.sync = false;
    free_p.rate_hz = 0.1f;
    CHECK(!chandas_is_capped(&free_p, 20.0f), "free mode capped");
}

static void a_long_grain_never_overtakes_the_write_head(void) {
    float buffer = CHANDAS_BUFFER_SECONDS * SR;
    const float delays[3] = {480.0f, 48000.0f, 16.0f * 48000.0f};
    const float rates[5] = {0.25f, 0.5f, 1.0f, 1.6f, 4.0f};
    for (int di = 0; di < 3; di++) {
        for (int ri = 0; ri < 5; ri++) {
            for (int rev = 0; rev < 2; rev++) {
                float delay = delays[di];
                float rate = rates[ri];
                bool reverse = rev == 1;
                float life = chandas_max_life(delay, rate, reverse, buffer);
                CHECK(life >= 8.0f && isfinite(life), "%g %g %d", delay, rate,
                      rev);
                float step = reverse ? -rate : rate;
                float gap_end = delay + (1.0f - step) * life;
                CHECK(gap_end > 0.0f && gap_end <= buffer,
                      "gap left the buffer: delay %g rate %g rev %d life %g "
                      "end %g",
                      delay, rate, rev, life, gap_end);
            }
        }
    }
}

static float *rdm_render(float spread) {
    Chandas h;
    eng(&h, on_(1.0f, spread));
    size_t warm = (size_t)(SR * 8.0f);
    for (size_t i = 0; i < warm; i++) {
        float x = sinf((float)i * 0.037f) * 0.5f;
        Stereo d = {x, x};
        chandas_process(&h, d);
    }
    float *out = (float *)malloc((size_t)SR * sizeof(float));
    for (size_t i = 0; i < (size_t)SR; i++) {
        Stereo d = {sinf((float)(i + warm) * 0.037f) * 0.5f, 0.0f};
        out[i] = chandas_process(&h, d).l;
    }
    chandas_free(&h);
    return out;
}

static void the_streams_read_different_material(void) {
    Chandas h;
    eng(&h, on_(CHANDAS_MAX_SIZE, 0.0f));
    float base = chandas_base_seconds(&h) * SR;
    for (size_t i = 0; i < (size_t)SR * 3; i++) {
        Stereo d = {0.0f, 0.0f};
        chandas_process(&h, d);
    }
    float *ir = (float *)malloc((size_t)SR * sizeof(float));
    for (size_t i = 0; i < (size_t)SR; i++) {
        Stereo d = {i == 0 ? 1.0f : 0.0f, 0.0f};
        ir[i] = fabsf(chandas_process(&h, d).l);
    }
    for (size_t k = 0; k < CHANDAS_STREAMS; k++) {
        size_t at = (size_t)roundf(base / chandas_subdivisions[k]);
        float arrival = fmaxf(fmaxf(ir[at - 1], ir[at]), ir[at + 1]);
        CHECK(arrival > 0.05f, "no arrival at base/%g: %.5f at sample %zu",
              chandas_subdivisions[k], arrival, at);
        size_t quiet_at = at + (size_t)(base / 8.0f);
        float quiet = ir[quiet_at];
        CHECK(arrival > quiet * 4.0f,
              "base/%g is not a distinct arrival: %.5f against %.5f",
              chandas_subdivisions[k], arrival, quiet);
    }
    free(ir);
    chandas_free(&h);

    float *taps = rdm_render(0.0f);
    float *grains = rdm_render(1.0f);
    float diff = 0.0f;
    for (size_t i = 0; i < (size_t)SR; i++) {
        diff = fmaxf(diff, fabsf(taps[i] - grains[i]));
    }
    CHECK(diff > 0.01f, "SPREAD did not scatter the reads: max diff %g", diff);
    free(taps);
    free(grains);
}

static void regeneration_always_decays(void) {
    const float sizes[3] = {CHANDAS_MIN_SIZE, 1.0f, CHANDAS_MAX_SIZE};
    const float onoff[2] = {0.0f, 1.0f};
    for (int si = 0; si < 3; si++) {
        for (int pi = 0; pi < 2; pi++) {
            for (int wi = 0; wi < 2; wi++) {
                float size = sizes[si];
                float spread = onoff[pi];
                float warp = onoff[wi];
                ChandasParams p = on_(size, spread);
                p.warp = warp;
                Chandas h;
                eng(&h, p);
                double rms[12];
                for (int s = 0; s < 12; s++) {
                    double sq = 0.0;
                    for (size_t i = 0; i < (size_t)SR; i++) {
                        float x = (s == 0 && i == 0) ? 0.5f : 0.0f;
                        Stereo d = {x, x};
                        Stereo o = chandas_process(&h, d);
                        CHECK(isfinite(o.l) && isfinite(o.r), "not finite");
                        sq += (double)o.l * (double)o.l;
                    }
                    rms[s] = sqrt(sq / (double)SR);
                }
                CHECK(rms[10] < rms[2] * 0.5 + 1e-9,
                      "size %g spread %g warp %g: rms was %.6f at 2 s and "
                      "%.6f at 10 s",
                      size, spread, warp, rms[2], rms[10]);
                chandas_free(&h);
            }
        }
    }
}

static void the_output_carries_no_dc(void) {
    Chandas h;
    eng(&h, on_(1.0f, 0.5f));
    size_t warm = (size_t)(SR * 10.0f);
    for (size_t i = 0; i < warm; i++) {
        float x = sinf((float)i * 0.05f) * 0.5f;
        Stereo d = {x, x};
        chandas_process(&h, d);
    }
    size_t n = (size_t)(SR * 6.0f);
    double sum = 0.0;
    for (size_t i = warm; i < warm + n; i++) {
        float x = sinf((float)i * 0.05f) * 0.5f;
        Stereo d = {x, x};
        sum += (double)chandas_process(&h, d).l;
    }
    double mean = fabs(sum / (double)n);
    CHECK(mean < 1e-3, "manufactured DC: mean %g", mean);
    chandas_free(&h);

    eng(&h, on_(1.0f, 0.5f));
    for (size_t i = 0; i < warm; i++) {
        float x = 0.02f + sinf((float)i * 0.05f) * 0.05f;
        Stereo d = {x, x};
        chandas_process(&h, d);
    }
    sum = 0.0;
    for (size_t i = warm; i < warm + n; i++) {
        float x = 0.02f + sinf((float)i * 0.05f) * 0.05f;
        Stereo d = {x, x};
        sum += (double)chandas_process(&h, d).l;
    }
    mean = fabs(sum / (double)n);
    CHECK(mean < 0.02, "input DC was amplified: 0.02 in, %g out", mean);
    chandas_free(&h);
}

static void silent_until_asked(void) {
    ChandasParams cases[2];
    cases[0] = chandas_params_default();
    cases[1] = chandas_params_default();
    cases[1].enabled = true;
    cases[1].mix = 0.0f;
    for (int c = 0; c < 2; c++) {
        Chandas h;
        eng(&h, cases[c]);
        for (int i = 0; i < 4096; i++) {
            float s = sinf((float)i * 0.01f);
            Stereo d = {s, -s};
            Stereo o = chandas_process(&h, d);
            CHECK(o.l == s && o.r == -s, "sample %d of case %d", i, c);
        }
        chandas_free(&h);
    }
}

static void stays_finite_and_bounded_at_the_extremes(void) {
    const size_t divisions[4] = {0, CHANDAS_DIVISIONS_LEN - 1,
                                 CHANDAS_DEFAULT_DIVISION,
                                 CHANDAS_DEFAULT_DIVISION};
    const float spreads[4] = {1.0f, 1.0f, 0.5f, 0.0f};
    const float sizes[4] = {CHANDAS_MAX_SIZE, CHANDAS_MIN_SIZE, 1.0f,
                            CHANDAS_MAX_SIZE};
    const float warps[4] = {1.0f, 1.0f, 0.5f, 0.0f};
    for (int c = 0; c < 4; c++) {
        Chandas h;
        chandas_init(&h, SR);
        chandas_set_tempo(&h, CHANDAS_MIN_BPM);
        ChandasParams p = chandas_params_default();
        p.enabled = true;
        p.mix = 1.0f;
        p.sync = true;
        p.division = divisions[c];
        p.spread = spreads[c];
        p.size = sizes[c];
        p.warp = warps[c];
        chandas_set_params(&h, p);
        float peak = 0.0f;
        for (int i = 0; i < 48000 * 4; i++) {
            float s = sinf((float)i * 0.05f) * 0.5f;
            Stereo d = {s, s};
            Stereo o = chandas_process(&h, d);
            CHECK(isfinite(o.l) && isfinite(o.r), "not finite at %d", i);
            peak = fmaxf(peak, fmaxf(fabsf(o.l), fabsf(o.r)));
        }
        CHECK(peak < 2.0f, "runaway: peak %g at division %zu", peak,
              divisions[c]);
        chandas_free(&h);
    }
}

static void the_image_never_collapses_into_one_channel(void) {
    const float sizes[3] = {CHANDAS_MIN_SIZE, 1.0f, CHANDAS_MAX_SIZE};
    const float spreads[3] = {0.0f, 0.5f, 1.0f};
    for (int si = 0; si < 3; si++) {
        for (int pi = 0; pi < 3; pi++) {
            float size = sizes[si];
            float spread = spreads[pi];
            Chandas h;
            chandas_init(&h, SR);
            chandas_set_tempo(&h, CHANDAS_MIN_BPM);
            ChandasParams p = chandas_params_default();
            p.enabled = true;
            p.mix = 1.0f;
            p.sync = true;
            p.division = CHANDAS_DEFAULT_DIVISION;
            p.spread = spread;
            p.size = size;
            p.warp = 0.5f;
            chandas_set_params(&h, p);
            size_t warm = 48000 * 12;
            for (size_t i = 0; i < warm; i++) {
                float x = sinf((float)i * 0.05f) * 0.5f;
                Stereo d = {x, x};
                chandas_process(&h, d);
            }
            double el = 0.0, er = 0.0;
            for (size_t i = warm; i < warm + 48000 * 6; i++) {
                float x = sinf((float)i * 0.05f) * 0.5f;
                Stereo d = {x, x};
                Stereo o = chandas_process(&h, d);
                el += (double)o.l * (double)o.l;
                er += (double)o.r * (double)o.r;
            }
            double lo = el < er ? el : er;
            double hi = el < er ? er : el;
            CHECK(lo > 0.0, "size %g spread %g: one channel fell silent", size,
                  spread);
            CHECK(hi / lo < 4.0, "size %g spread %g: %.1f:1", size, spread,
                  hi / lo);
            chandas_free(&h);
        }
    }
}

static float *voices_render(float spread) {
    ChandasParams p = on_(1.0f, spread);
    p.rate_hz = 4.0f;
    Chandas h;
    eng(&h, p);
    float *out = (float *)malloc(96000 * sizeof(float));
    for (size_t i = 0; i < 96000; i++) {
        Stereo d = {sinf((float)i * 0.02f) * 0.5f, 0.0f};
        out[i] = chandas_process(&h, d).l;
    }
    chandas_free(&h);
    return out;
}

static float rms_f32(const float *v, size_t n) {
    float sum = 0.0f;
    for (size_t i = 0; i < n; i++) {
        sum += v[i] * v[i];
    }
    return sqrtf(sum / (float)n);
}

static void it_voices_and_spread_changes_what_is_voiced(void) {
    float *flat = voices_render(0.0f);
    float *wide = voices_render(1.0f);
    CHECK(rms_f32(flat + 48000, 48000) > 0.01f, "silent: rms %g",
          rms_f32(flat + 48000, 48000));
    CHECK(rms_f32(wide + 48000, 48000) > 0.01f, "silent at full spread");
    float diff = 0.0f;
    for (size_t i = 48000; i < 96000; i++) {
        diff = fmaxf(diff, fabsf(flat[i] - wide[i]));
    }
    CHECK(diff > 0.01f, "spread changed nothing: max diff %g", diff);
    free(flat);
    free(wide);
}

static void mix_glides_rather_than_stepping(void) {
    Chandas h;
    chandas_init(&h, SR);
    ChandasParams p = on_(1.0f, 0.5f);
    p.mix = 0.0f;
    chandas_set_params(&h, p);
    size_t warm = (size_t)(SR * 10.0f);
    for (size_t i = 0; i < warm; i++) {
        float t = sinf((float)i * 0.02f) * 0.5f;
        Stereo d = {t, t};
        chandas_process(&h, d);
    }
    float t0 = sinf((float)warm * 0.02f) * 0.5f;
    Stereo d0 = {t0, t0};
    float prev = chandas_process(&h, d0).l;
    p.mix = 1.0f;
    chandas_set_params(&h, p);
    float worst = 0.0f;
    for (size_t i = warm + 1; i < warm + 4096; i++) {
        float t = sinf((float)i * 0.02f) * 0.5f;
        Stereo d = {t, t};
        float l = chandas_process(&h, d).l;
        worst = fmaxf(worst, fabsf(l - prev));
        prev = l;
    }
    CHECK(worst < 0.05f, "MIX stepped: worst single-sample jump %g", worst);
    chandas_free(&h);
}

static void spread_cannot_step_a_sounding_grain(void) {
    Chandas h;
    chandas_init(&h, SR);
    ChandasParams p = on_(1.0f, 0.0f);
    p.spread = 0.0f;
    chandas_set_params(&h, p);
    size_t warm = (size_t)(SR * 10.0f);
    for (size_t i = 0; i < warm; i++) {
        float t = sinf((float)i * 0.02f) * 0.5f;
        Stereo d = {t, t};
        chandas_process(&h, d);
    }
    float t0 = sinf((float)warm * 0.02f) * 0.5f;
    Stereo d0 = {t0, t0};
    float prev = chandas_process(&h, d0).l;
    p.spread = 1.0f;
    chandas_set_params(&h, p);
    float worst = 0.0f;
    for (size_t i = warm + 1; i < warm + 4096; i++) {
        float t = sinf((float)i * 0.02f) * 0.5f;
        Stereo d = {t, t};
        float l = chandas_process(&h, d).l;
        worst = fmaxf(worst, fabsf(l - prev));
        prev = l;
    }
    CHECK(worst < 0.05f, "SPREAD stepped: worst single-sample jump %g", worst);
    chandas_free(&h);
}

static void the_pool_fits_by_construction(void) {
    CHECK(CHANDAS_MAX_SIZE == 2.0f, "MAX_SIZE");
    CHECK(CHANDAS_MAX_GRAINS == CHANDAS_STREAMS * 2, "MAX_GRAINS");
    ChandasParams p = on_(CHANDAS_MAX_SIZE, 1.0f);
    p.rate_hz = 8.0f;
    p.warp = 0.5f;
    Chandas h;
    eng(&h, p);
    for (int i = 0; i < 48000 * 4; i++) {
        Stereo d = {sinf((float)i * 0.05f) * 0.5f, 0.0f};
        chandas_process(&h, d);
        int live = 0;
        for (size_t g = 0; g < CHANDAS_MAX_GRAINS; g++) {
            if (h.grains[g].active) {
                live++;
            }
        }
        CHECK(live <= CHANDAS_MAX_GRAINS, "%d grains alive, pool is %d", live,
              CHANDAS_MAX_GRAINS);
    }
    chandas_free(&h);
}

static void the_streams_fill_the_field(void) {
    Chandas h;
    chandas_init(&h, SR);
    for (size_t k = 0; k < CHANDAS_STREAMS; k++) {
        CHECK(h.tap_pan[k] >= 0.0f && h.tap_pan[k] <= 1.0f,
              "pan %g off the field", h.tap_pan[k]);
    }
    float first = h.tap_pan[0];
    float last = h.tap_pan[CHANDAS_STREAMS - 1];
    CHECK(fabsf(first - (0.5f - CHANDAS_TAP_SPREAD * 0.5f)) < 1e-5f, "%g",
          first);
    CHECK(fabsf(last - (0.5f + CHANDAS_TAP_SPREAD * 0.5f)) < 1e-5f, "%g", last);
    CHECK(first > 0.0f && last < 1.0f, "a stream is hard-panned");
    CHECK(fabsf(h.tap_pan[1] - 0.5f) < 1e-5f, "%g", h.tap_pan[1]);
    chandas_free(&h);
}

static float *same_render(void) {
    ChandasParams p = on_(1.0f, 0.6f);
    p.warp = 0.5f;
    Chandas h;
    eng(&h, p);
    float *out = (float *)malloc(48000 * sizeof(float));
    for (size_t i = 0; i < 48000; i++) {
        Stereo d = {sinf((float)i * 0.03f), 0.0f};
        out[i] = chandas_process(&h, d).l;
    }
    chandas_free(&h);
    return out;
}

static void the_same_settings_give_the_same_instrument_twice(void) {
    float *a = same_render();
    float *b = same_render();
    bool equal = true;
    for (size_t i = 0; i < 48000; i++) {
        if (a[i] != b[i]) {
            equal = false;
            break;
        }
    }
    CHECK(equal, "renders differ");
    free(a);
    free(b);
}

static void warp_reaches_both_of_its_ends(void) {
    size_t forwards = 0, backwards = 0, half = 0;
    for (uint32_t n = 1; n < 2000; n++) {
        if (chandas_dice(n) < 0.0f) {
            forwards++;
        }
        if (chandas_dice(n) < 1.0f) {
            backwards++;
        }
        if (chandas_dice(n) < 0.5f) {
            half++;
        }
    }
    CHECK(forwards == 0, "forwards %zu", forwards);
    CHECK(backwards == 1999, "backwards %zu", backwards);
    CHECK(half >= 800 && half < 1200, "warp dice biased: %zu/1999", half);
}

static void the_streams_are_three_and_polyrhythmic(void) {
    CHECK(chandas_subdivisions[0] == 1.0f && chandas_subdivisions[1] == 2.0f &&
              chandas_subdivisions[2] == 3.0f,
          "subdivisions");
    CHECK(CHANDAS_STREAMS == 3, "streams");
    Chandas h;
    chandas_init(&h, SR);
    CHECK(CHANDAS_STREAMS == 3, "streams of instance");
    chandas_free(&h);
}

static void a_reset_clears_the_memory_and_ramps_back(void) {
    size_t warm = (size_t)(SR * 6.0f);
    size_t fade = (size_t)ceilf(CHANDAS_RESET_FADE_SECONDS * SR) + 2;

    Chandas h;
    eng(&h, on_(1.0f, 0.5f));
    for (size_t i = 0; i < warm; i++) {
        float t = sinf((float)i * 0.05f) * 0.5f;
        Stereo d = {t, t};
        chandas_process(&h, d);
    }
    float t0 = sinf((float)warm * 0.05f) * 0.5f;
    Stereo d0 = {t0, t0};
    float prev = chandas_process(&h, d0).l;
    chandas_reset(&h);
    float worst = 0.0f;
    for (size_t i = 1; i < fade; i++) {
        float t = sinf((float)(warm + i) * 0.05f) * 0.5f;
        Stereo d = {t, t};
        float l = chandas_process(&h, d).l;
        worst = fmaxf(worst, fabsf(l - prev));
        prev = l;
    }
    CHECK(worst < 0.02f, "the reset cut rather than faded: worst step %g",
          worst);
    chandas_free(&h);

    eng(&h, on_(1.0f, 0.5f));
    for (size_t i = 0; i < warm; i++) {
        float t = sinf((float)i * 0.05f) * 0.5f;
        Stereo d = {t, t};
        chandas_process(&h, d);
    }
    chandas_reset(&h);
    for (size_t i = 0; i < fade; i++) {
        Stereo d = {0.0f, 0.0f};
        chandas_process(&h, d);
    }
    for (size_t i = 0; i < (size_t)SR; i++) {
        Stereo d = {0.0f, 0.0f};
        Stereo o = chandas_process(&h, d);
        CHECK(o.l == 0.0f && o.r == 0.0f, "the old preset survived, sample %zu",
              i);
    }
    float peak = 0.0f;
    for (size_t i = 0; i < (size_t)SR * 4; i++) {
        float x = sinf((float)i * 0.05f) * 0.5f;
        Stereo d = {x, x};
        peak = fmaxf(peak, fabsf(chandas_process(&h, d).l));
    }
    CHECK(peak > 0.05f, "the wet never came back: peak %g", peak);
    chandas_free(&h);
}

static void reenabling_after_a_sit_does_not_step(void) {
    size_t warm = (size_t)(SR * 6.0f);
    Chandas h;
    eng(&h, on_(1.0f, 0.5f));
    for (size_t i = 0; i < warm; i++) {
        float t = sinf((float)i * 0.05f) * 0.5f;
        Stereo d = {t, t};
        chandas_process(&h, d);
    }
    ChandasParams p = chandas_params(&h);
    p.enabled = false;
    chandas_set_params(&h, p);
    size_t sit = (size_t)(SR * 0.5f);
    for (size_t i = 0; i < sit; i++) {
        float t = sinf((float)(warm + i) * 0.05f) * 0.5f;
        Stereo d = {t, t};
        chandas_process(&h, d);
    }
    float t0 = sinf((float)(warm + sit) * 0.05f) * 0.5f;
    Stereo d0 = {t0, t0};
    float prev = chandas_process(&h, d0).l;
    p.enabled = true;
    chandas_set_params(&h, p);
    float worst = 0.0f;
    for (size_t i = warm + sit + 1; i < warm + sit + 4096; i++) {
        float t = sinf((float)i * 0.05f) * 0.5f;
        Stereo d = {t, t};
        float l = chandas_process(&h, d).l;
        worst = fmaxf(worst, fabsf(l - prev));
        prev = l;
    }
    CHECK(worst < 0.05f, "re-enabling stepped: worst single-sample jump %g",
          worst);
    chandas_free(&h);
}

static void sync_falls_back_to_the_transport_before_the_first_pulse(void) {
    Chandas h;
    chandas_init(&h, SR);
    chandas_set_tempo(&h, 120.0f);
    ChandasParams p = chandas_params_default();
    p.sync = true;
    p.division = CHANDAS_DEFAULT_DIVISION;
    chandas_set_params(&h, p);
    float beats = CHANDAS_DIVISIONS[CHANDAS_DEFAULT_DIVISION].beats;
    float want = beats * 60.0f / 120.0f;
    CHECK(fabsf(chandas_base_seconds(&h) - want) < 1e-6f,
          "expected %g, got %g", want, chandas_base_seconds(&h));
    chandas_free(&h);
}

static void sync_locks_to_the_measured_harmony_interval(void) {
    Chandas h;
    chandas_init(&h, SR);
    chandas_set_tempo(&h, 120.0f);
    ChandasParams p = chandas_params_default();
    p.sync = true;
    p.division = CHANDAS_DEFAULT_DIVISION;
    chandas_set_params(&h, p);
    size_t gap = (size_t)(SR * 0.3f);
    chandas_note_pulse(&h);
    for (size_t i = 0; i < gap; i++) {
        Stereo d = {0.0f, 0.0f};
        chandas_process(&h, d);
    }
    chandas_note_pulse(&h);
    float beats = CHANDAS_DIVISIONS[CHANDAS_DEFAULT_DIVISION].beats;
    float want = beats * 0.3f;
    CHECK(fabsf(chandas_base_seconds(&h) - want) < 1e-3f,
          "expected %g, got %g", want, chandas_base_seconds(&h));
    chandas_free(&h);
}

void test_chandas(void) {
    the_division_ladder_runs_from_8_1_down_to_1_32();
    no_setting_can_outrun_the_buffer();
    the_longest_rung_reports_when_it_is_capped();
    a_long_grain_never_overtakes_the_write_head();
    the_streams_read_different_material();
    regeneration_always_decays();
    the_output_carries_no_dc();
    silent_until_asked();
    stays_finite_and_bounded_at_the_extremes();
    the_image_never_collapses_into_one_channel();
    it_voices_and_spread_changes_what_is_voiced();
    mix_glides_rather_than_stepping();
    spread_cannot_step_a_sounding_grain();
    the_pool_fits_by_construction();
    the_streams_fill_the_field();
    the_same_settings_give_the_same_instrument_twice();
    warp_reaches_both_of_its_ends();
    the_streams_are_three_and_polyrhythmic();
    a_reset_clears_the_memory_and_ramps_back();
    reenabling_after_a_sit_does_not_step();
    sync_falls_back_to_the_transport_before_the_first_pulse();
    sync_locks_to_the_measured_harmony_interval();
}
