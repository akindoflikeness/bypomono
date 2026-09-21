#include "../src/dsp/dsp.h"
#include "test.h"
#include <math.h>
#include <stdint.h>

#define SR 48000.0f

static void noop_frame(void *userdata, size_t n, const Frame *frame) {}

static Frame impulse_frame(float v) {
    Frame f;
    for (int i = 0; i < NUM_OPS; i++) {
        f.ops[i] = v;
    }
    f.mix = v;
    f.master = 1.0f;
    f.field = 0.0f;
    f.base_hz = 50.0f;
    f.side = 0.0f;
    return f;
}

static void configured(StereoVerb *verb, RatioMode mode) {
    Patch patch = patch_init(ALGORITHMS[0], mode);
    Compiled compiled = compile(patch.algorithm);
    verb_init(verb, SR);
    verb_configure(verb, &patch, &compiled);
}

static float gain_at(float hz, float g) {
    Svf svf = { 0.0f, 0.0f };
    size_t n = (size_t)SR;
    float peak = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float x = sinf(TAU_F * hz * (float)i / SR);
        float y = svf_process_hp(&svf, x, g, VERB_ROOM_HP_K);
        if (i > n / 2) {
            peak = fmaxf(peak, fabsf(y));
        }
    }
    return peak;
}

static void the_room_high_pass_rolls_off_on_a_shoulder_not_a_corner(void) {
    float sr = 48000.0f;
    float corner = verb_room_hp_hz(300.0f);
    CHECK(fabsf(corner - VERB_ROOM_HP_MAX_HZ) < 1e-3f, "corner %g", corner);
    float g = tanf(PI_F * corner / sr);

    float q = 1.0f / VERB_ROOM_HP_K;
    float at_corner = gain_at(corner, g);
    CHECK(fabsf(at_corner - q) < 0.02f,
          "gain at the %g hz corner is %g, expected Q = %g", corner, at_corner, q);
    CHECK(at_corner < VERB_Q_BASE,
          "the corner must sit below Butterworth, or it is not negative resonance");
    float at20 = gain_at(20.0f, g);
    CHECK(at20 < 0.06f, "20 hz should be gone: %g", at20);
    float at2k = gain_at(2000.0f, g);
    CHECK(fabsf(at2k - 1.0f) < 0.02f, "2 khz should pass untouched: %g", at2k);
}

static void the_rooms_floor_always_sits_under_the_note(void) {
    for (int hzi = 1; hzi <= 4000; hzi++) {
        float hz = (float)hzi;
        float corner = verb_room_hp_hz(hz);
        CHECK(corner >= VERB_ROOM_HP_MIN_HZ && corner <= VERB_ROOM_HP_MAX_HZ,
              "%g hz cornered at %g", hz, corner);
        if (hz > VERB_ROOM_HP_MIN_HZ * PHI && hz < VERB_ROOM_HP_MAX_HZ * PHI) {
            CHECK(fabsf(corner - hz / PHI) < 1e-3f,
                  "%g hz cornered at %g, wanted %g", hz, corner, hz / PHI);
        }
        if (hz >= VERB_ROOM_HP_MIN_HZ * PHI) {
            CHECK(corner < hz, "%g hz cornered at or above itself: %g", hz, corner);
        }
    }
    float at_init = verb_room_hp_hz(50.0f);
    CHECK(fabsf(at_init - 30.9f) < 0.1f, "the 50 hz drone corners at %g", at_init);
    CHECK(at_init < 50.0f / 1.5f, "the floor is crowding the fundamental");
    CHECK(fabsf(at_init - 25.0f) > 3.0f, "the floor landed on the sub-octave");
}

static void damp_resonance_is_reachable_before_the_top_of_the_knob(void) {
    CHECK(fabsf(verb_damp_q(0.0f) - VERB_Q_BASE) < 1e-6f, "q at 0: %g", verb_damp_q(0.0f));
    CHECK(fabsf(verb_damp_q(VERB_Q_KNEE * 0.99f) - VERB_Q_BASE) < 1e-6f,
          "q below knee: %g", verb_damp_q(VERB_Q_KNEE * 0.99f));
    CHECK(verb_damp_q(VERB_Q_KNEE + 0.05f) > VERB_Q_BASE,
          "q above knee: %g", verb_damp_q(VERB_Q_KNEE + 0.05f));
    CHECK(fabsf(verb_damp_q(1.0f) - powi_f(PHI, 4)) < 1e-4f, "q at 1: %g", verb_damp_q(1.0f));

    CHECK(VERB_Q_KNEE < 1.0f / PHI, "the knee climbed back up a step");
    CHECK(verb_damp_q(0.5f) > VERB_Q_BASE, "half the knob is still a plain filter");

    float mid = verb_damp_q((VERB_Q_KNEE + 1.0f) / 2.0f);
    CHECK(mid < (VERB_Q_BASE + powi_f(PHI, 4)) / 2.0f,
          "the rise went linear or concave: %g", mid);
    float last = 0.0f;
    for (int i = 0; i <= 100; i++) {
        float q = verb_damp_q((float)i / 100.0f);
        CHECK(q >= last - 1e-6f, "resonance dipped at damp %g", (float)i / 100.0f);
        last = q;
    }
}

static void silence_in_silence_out(void) {
    StereoVerb verb;
    configured(&verb, RATIO_GOLDEN);
    for (int i = 0; i < 48000; i++) {
        Frame f = impulse_frame(0.0f);
        Stereo s = verb_process(&verb, &f);
        CHECK(s.l == 0.0f && s.r == 0.0f, "not silent: %g %g", s.l, s.r);
    }
    verb_free(&verb);
}

static void impulse_tail_decays_and_stays_finite(void) {
    for (int mode = 0; mode < RATIO_MODE_COUNT; mode++) {
        StereoVerb verb;
        configured(&verb, (RatioMode)mode);
        VerbParams p = verb_params_default();
        p.decay = 0.5f;
        verb_set_params(&verb, p);
        double early = 0.0, late = 0.0;
        for (int n = 0; n < 96000; n++) {
            float x = n == 0 ? 1.0f : 0.0f;
            Frame f = impulse_frame(x);
            Stereo s = verb_process(&verb, &f);
            CHECK(isfinite(s.l) && isfinite(s.r), "mode %d at sample %d", mode, n);
            double e = (double)(s.l * s.l + s.r * s.r);
            if (n < 24000) {
                early += e;
            } else if (n >= 72000) {
                late += e;
            }
        }
        CHECK(early > 0.0, "mode %d: no tail at all", mode);
        CHECK(late < early / 100.0,
              "mode %d: tail not decaying (early %g, late %g)", mode, early, late);
        verb_free(&verb);
    }
}

static void the_two_ears_decorrelate(void) {
    StereoVerb verb;
    configured(&verb, RATIO_GOLDEN);
    size_t differ = 0;
    for (int n = 0; n < 48000; n++) {
        float x = n == 0 ? 1.0f : 0.0f;
        Frame f = impulse_frame(x);
        Stereo s = verb_process(&verb, &f);
        if (fabsf(s.l - s.r) > 1e-9f) {
            differ++;
        }
    }
    CHECK(differ > 10000, "ears too similar: %zu differing samples", differ);
    verb_free(&verb);
}

static void ghost_allpass_rotates_the_design_pitch_by_pi_over_five(void) {
    float a = allpass_coeff_for(START_HZ, SR, PHASE_PER_PASS);
    float w = TAU_F * START_HZ / SR;
    float phase = allpass_phase(a, w);
    CHECK(fabsf(phase + PHASE_PER_PASS) < 1e-3f,
          "solved a = %g, phase = %g (want %g)", a, phase, -PHASE_PER_PASS);
}

static void rotators_follow_the_drone_pitch(void) {
    float at_start = allpass_coeff_for(START_HZ, SR, PHASE_PER_PASS);
    const float hz[2] = { 55.0f, 220.0f };

    StereoVerb verb;
    configured(&verb, RATIO_GOLDEN);
    VoicePair pair;
    voice_pair_init(&pair, SR, patch_init(ALGORITHMS[0], RATIO_GOLDEN));
    CHECK(verb.ghosts[0].rot.a == at_start, "ghost starts off the drone pitch");
    CHECK(pair.voices[0].rip_line.rot.a == at_start, "rip starts off the drone pitch");

    /* the pitch sets where the rotators are going; they travel there, because
       writing a live filter's coefficient steps its output */
    for (int k = 0; k < 2; k++) {
        float want = allpass_coeff_for(hz[k], SR, PHASE_PER_PASS);
        verb_set_drone_hz(&verb, hz[k]);
        voice_pair_set_drone_hz(&pair, hz[k]);
        CHECK(want != at_start, "%g hz solves to the 110 hz coefficient", (double)hz[k]);
        for (int i = 0; i < NUM_OPS; i++)
            CHECK(verb.ghosts[i].rot_to == want, "ghost %d at %g hz aims at %g (want %g)", i,
                  (double)hz[k], (double)verb.ghosts[i].rot_to, (double)want);
        for (int i = 0; i < 2; i++)
            CHECK(pair.voices[i].rip_line.rot_to == want, "rip %d at %g hz aims at %g (want %g)",
                  i, (double)hz[k], (double)pair.voices[i].rip_line.rot_to, (double)want);
        Frame quiet = impulse_frame(0.0f);
        for (size_t n = 0; n < (size_t)(SR * 0.2f); n++) verb_process(&verb, &quiet);
        voice_pair_render_frames(&pair, (size_t)(SR * 0.2f), noop_frame, NULL);
        for (int i = 0; i < NUM_OPS; i++)
            CHECK(fabsf(verb.ghosts[i].rot.a - want) < 1e-3f, "ghost %d never arrived: %g",
                  i, (double)verb.ghosts[i].rot.a);
        for (int i = 0; i < 2; i++)
            CHECK(fabsf(pair.voices[i].rip_line.rot.a - want) < 1e-3f, "rip %d never arrived: %g",
                  i, (double)pair.voices[i].rip_line.rot.a);
    }
    voice_pair_free(&pair);
    verb_free(&verb);
}

static void haunt_zero_leaves_the_room_untouched_and_full_haunt_is_stable(void) {
    StereoVerb plain, zeroed;
    configured(&plain, RATIO_GOLDEN);
    configured(&zeroed, RATIO_GOLDEN);
    VerbParams p = verb_params(&zeroed);
    p.haunt = 0.0f;
    verb_set_params(&zeroed, p);
    bool differs = false;
    for (int n = 0; n < 9600; n++) {
        float x = n % 480 == 0 ? 0.5f : 0.0f;
        Frame f = impulse_frame(x);
        Stereo a = verb_process(&plain, &f);
        Stereo b = verb_process(&zeroed, &f);
        if (a.l != b.l || a.r != b.r) {
            differs = true;
        }
    }
    CHECK(!differs, "haunt 0 changed the approved Room sound");
    verb_free(&plain);
    verb_free(&zeroed);

    StereoVerb haunted, reference;
    configured(&haunted, RATIO_GOLDEN);
    VerbParams hp = verb_params(&haunted);
    hp.haunt = 1.0f;
    verb_set_params(&haunted, hp);
    configured(&reference, RATIO_GOLDEN);
    bool diverged = false;
    for (int n = 0; n < 192000; n++) {
        float x = sinf(TAU_F * 110.0f * (float)n / SR) * 0.5f;
        Frame f = impulse_frame(x);
        Stereo s = verb_process(&haunted, &f);
        CHECK(isfinite(s.l) && isfinite(s.r), "haunt blew up at %d", n);
        CHECK(fabsf(s.l) < 10.0f && fabsf(s.r) < 10.0f, "haunt runaway at %d", n);
        Stereo ref = verb_process(&reference, &f);
        if (s.l != ref.l || s.r != ref.r) {
            diverged = true;
        }
    }
    CHECK(diverged, "haunt 1.0 made no difference at all");
    verb_free(&haunted);
    verb_free(&reference);
}

static double rms_at_decay(float decay) {
    StereoVerb verb;
    configured(&verb, RATIO_GOLDEN);
    VerbParams p = verb_params_default();
    p.mix = 1.0f;
    p.decay = decay;
    verb_set_params(&verb, p);
    double acc = 0.0;
    for (int n = 0; n < 192000; n++) {
        float x = sinf(TAU_F * 110.0f * (float)n / SR) * 0.3f;
        Frame f = impulse_frame(x);
        Stereo s = verb_process(&verb, &f);
        if (n >= 96000) {
            acc += (double)(s.l * s.l + s.r * s.r);
        }
    }
    verb_free(&verb);
    return sqrt(acc / 96000.0);
}

static void longer_decay_is_never_quieter(void) {
    double short_ = rms_at_decay(1.0f);
    double long_ = rms_at_decay(8.0f);
    CHECK(long_ >= short_ * 0.8,
          "long decay must not be quieter (short %g, long %g)", short_, long_);
}

static float measured_t60(float decay, float damp) {
    StereoVerb verb;
    configured(&verb, RATIO_GOLDEN);
    VerbParams p = verb_params_default();
    p.mix = 1.0f;
    p.decay = decay;
    p.damp = damp;
    p.haunt = 0.0f;
    verb_set_params(&verb, p);
    double level0 = 0.0;
    for (int n = 0; n < 96000; n++) {
        float x = sinf(TAU_F * 65.0f * (float)n / SR) * 0.5f;
        Frame f = impulse_frame(x);
        Stereo s = verb_process(&verb, &f);
        level0 = level0 * (1.0 - 1e-4) + 1e-4 * (double)(0.5f * (s.l * s.l + s.r * s.r));
    }
    double target = level0 * 1e-6;
    for (int w = 0; w < 100; w++) {
        double acc = 0.0;
        for (int i = 0; i < 4800; i++) {
            Frame f = impulse_frame(0.0f);
            Stereo s = verb_process(&verb, &f);
            acc += (double)(0.5f * (s.l * s.l + s.r * s.r));
        }
        if (acc / 4800.0 <= target) {
            verb_free(&verb);
            return (float)(w + 1) * 0.1f;
        }
    }
    verb_free(&verb);
    return INFINITY;
}

typedef struct {
    StereoVerb *verb;
    int pass;
    float peak;
    double acc;
    uint64_t n;
} DecayCtx;

static void decay_emit(void *userdata, size_t idx, const Frame *frame) {
    (void)idx;
    DecayCtx *ctx = (DecayCtx *)userdata;
    Stereo s = verb_process(ctx->verb, frame);
    if (ctx->pass == 4) {
        ctx->peak = fmaxf(fmaxf(ctx->peak, fabsf(s.l)), fabsf(s.r));
        ctx->acc += (double)(0.5f * (s.l * s.l + s.r * s.r));
        ctx->n += 1;
    }
}

static double rms_at_decay_voiced(float decay) {
    Patch patch = patch_init(ALGORITHMS[0], RATIO_FIBONACCI);
    patch.feedback = 0.71f;
    patch.index = 0.241f;
    patch.rip = 0.2f;
    patch.master_level = 0.8f;
    patch.field = 1.0f;
    patch.curve = 0.73f;
    Voice v;
    voice_init(&v, SR, patch);
    voice_set_freq_hz(&v, 50.0f);
    voice_note_on(&v, 50.0f, 1.0f); /* held: the default envelope sustains at 1 */
    StereoVerb verb;
    verb_init(&verb, SR);
    Compiled compiled = compile(patch.algorithm);
    verb_configure(&verb, &patch, &compiled);
    VerbParams p = verb_params_default();
    p.mix = 1.0f;
    p.decay = decay;
    p.damp = 0.55f;
    p.haunt = 0.0f;
    verb_set_params(&verb, p);
    DecayCtx ctx = { &verb, 0, 0.0f, 0.0, 0 };
    size_t sec = (size_t)SR;
    for (int pass = 0; pass < 5; pass++) {
        ctx.pass = pass;
        voice_render_frames(&v, sec, decay_emit, &ctx);
    }
    double out = sqrt(ctx.acc / (double)(ctx.n > 1 ? ctx.n : 1));
    voice_free(&v);
    verb_free(&verb);
    return out;
}

static void decay_lengthens_the_tail_without_squaring_the_room(void) {
    double short_ = rms_at_decay_voiced(2.0f);
    double long_ = rms_at_decay_voiced(8.0f);
    CHECK(long_ <= short_ * 1.25,
          "decay is acting as a drive control: wet rms %.4f at 2 s, %.4f at 8 s",
          short_, long_);
}

static void commanded_decay_is_real_and_damp_invariant(void) {
    float plain = measured_t60(2.0f, 0.0f);
    float damped = measured_t60(2.0f, 0.9f);
    CHECK(plain >= 1.5f && plain <= 2.8f, "commanded 2 s measured %g s", plain);
    CHECK(fabsf(damped - plain) <= 0.3f,
          "damp moved the decay: %g s plain vs %g s damped", plain, damped);
}

static void resonant_damp_screams_but_never_runs_away(void) {
    StereoVerb verb;
    configured(&verb, RATIO_GOLDEN);
    VerbParams p = verb_params_default();
    p.mix = 1.0f;
    p.decay = 8.0f;
    p.damp = 0.99f;
    p.haunt = 1.0f;
    verb_set_params(&verb, p);
    for (int n = 0; n < 480000; n++) {
        float x = sinf(TAU_F * 65.0f * (float)n / SR) * 0.8f;
        Frame f = impulse_frame(x);
        Stereo s = verb_process(&verb, &f);
        CHECK(isfinite(s.l) && isfinite(s.r), "blew up at %d", n);
        CHECK(fabsf(s.l) < 10.0f && fabsf(s.r) < 10.0f, "runaway at %d", n);
    }
    verb_free(&verb);
}

static void every_mode_and_algorithm_configures_within_buffers(void) {
    for (int mode = 0; mode < RATIO_MODE_COUNT; mode++) {
        for (int a = 0; a < 8; a++) {
            AlgorithmId alg = ALGORITHMS[a];
            Patch patch = patch_init(alg, (RatioMode)mode);
            StereoVerb verb;
            verb_init(&verb, SR);
            Compiled compiled = compile(alg);
            verb_configure(&verb, &patch, &compiled);
            for (int i = 0; i < NUM_OPS; i++) {
                CHECK(verb.combs_l[i].delay >= 1 && verb.combs_l[i].delay < verb.combs_l[i].len,
                      "comb_l %d delay %zu", i, verb.combs_l[i].delay);
                CHECK(verb.combs_r[i].delay >= 1 && verb.combs_r[i].delay < verb.combs_r[i].len,
                      "comb_r %d delay %zu", i, verb.combs_r[i].delay);
            }
            for (int i = 0; i < NUM_OPS; i++) {
                CHECK(verb.ghosts[i].delay >= 1 && verb.ghosts[i].delay < verb.ghosts[i].len,
                      "ghost %d delay %zu", i, verb.ghosts[i].delay);
            }
            verb_free(&verb);
        }
    }
}

void test_reverb(void) {
    the_room_high_pass_rolls_off_on_a_shoulder_not_a_corner();
    the_rooms_floor_always_sits_under_the_note();
    damp_resonance_is_reachable_before_the_top_of_the_knob();
    silence_in_silence_out();
    impulse_tail_decays_and_stays_finite();
    the_two_ears_decorrelate();
    ghost_allpass_rotates_the_design_pitch_by_pi_over_five();
    rotators_follow_the_drone_pitch();
    haunt_zero_leaves_the_room_untouched_and_full_haunt_is_stable();
    longer_decay_is_never_quieter();
    decay_lengthens_the_tail_without_squaring_the_room();
    commanded_decay_is_real_and_damp_invariant();
    resonant_damp_screams_but_never_runs_away();
    every_mode_and_algorithm_configures_within_buffers();
}
