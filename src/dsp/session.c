#include "dsp.h"

Session session_default(void) {
    Session s;
    s.patch = patch_init(ALGORITHMS[0], RATIO_GOLDEN);
    s.verb = verb_params_default();
    s.melody = melody_params_default();
    s.drone_hz = START_HZ;
    s.chandas = chandas_params_default();
    s.tempo_bpm = CHANDAS_DEFAULT_BPM;
    s.warmth = 0.5f;
    s.limiter_enabled = true;
    s.limiter_ceiling_db = LIMITER_CEILING_DB_DEFAULT;
    s.output_gain_db = OUTPUT_GAIN_DB_DEFAULT;
    EnvParams env = env_params_default();
    s.attack_s = env.attack_s;
    s.decay_s = env.decay_s;
    s.sustain = env.sustain;
    s.release_s = env.release_s;
    s.drone = false;
    s.mods = mod_bank_default();
    s.pitch = pitch_seq_params_default();
    return s;
}

Session session_sanitize(Session s) {
    bool known = false;
    for (int i = 0; i < 8; i++) {
        if (ALGORITHMS[i] == s.patch.algorithm) {
            known = true;
            break;
        }
    }
    if (!known) {
        s.patch.algorithm = ALGORITHMS[0];
    }
    if ((unsigned)s.patch.ratio_mode >= (unsigned)RATIO_MODE_COUNT)
        s.patch.ratio_mode = RATIO_GOLDEN;
    if ((unsigned)s.melody.tuning >= (unsigned)NUM_TUNINGS)
        s.melody.tuning = TUNING_SCALE;
    if ((unsigned)s.melody.scale >= (unsigned)NUM_SCALES)
        s.melody.scale = SCALE_PHRYGIAN;
    if ((unsigned)s.melody.source > (unsigned)HOLD_XORSHIFT)
        s.melody.source = HOLD_GOLDEN_WEYL;
    s.patch.feedback = clampf(s.patch.feedback, 0.0f, 1.0f);
    s.patch.index = clampf(s.patch.index, 0.0f, 1.0f);
    s.patch.rip = clampf(s.patch.rip, 0.0f, 1.0f);
    s.patch.master_level = clampf(s.patch.master_level, 0.0f, 1.0f);
    s.patch.glide_seconds = clampf(s.patch.glide_seconds, 0.0f, 30.0f);
    s.patch.voices = s.patch.voices < 1 ? 1 : (s.patch.voices > POLY_MAX ? POLY_MAX : s.patch.voices);
    s.patch.unison = s.patch.unison < 1 ? 1 : (s.patch.unison > UNISON_MAX ? UNISON_MAX : s.patch.unison);
    s.patch.unison_detune = clampf(s.patch.unison_detune, 0.0f, UNISON_DETUNE_MAX);
    for (int i = 0; i < NUM_OPS; i++) {
        s.patch.ops[i].level = clampf(s.patch.ops[i].level, 0.0f, 1.0f);
        s.patch.ops[i].ratio = clampf(s.patch.ops[i].ratio, OP_RATIO_MIN,
                                      OP_RATIO_MAX);
        s.patch.ops[i].detune_cents = clampf(s.patch.ops[i].detune_cents, -1200.0f, 1200.0f);
    }
    s.verb.mix = clampf(s.verb.mix, 0.0f, 1.0f);
    s.verb.ghost = clampf(s.verb.ghost, 0.0f, 1.0f);
    s.verb.decay = clampf(s.verb.decay, 0.05f, 8.0f);
    s.verb.damp = clampf(s.verb.damp, 0.0f, 0.99f);
    s.verb.haunt = clampf(s.verb.haunt, 0.0f, 1.0f);
    s.melody.rate_hz = clampf(s.melody.rate_hz, 0.1f, 8.0f);
    s.melody.range_degrees =
        s.melody.range_degrees < 1 ? 1 : (s.melody.range_degrees > 13 ? 13 : s.melody.range_degrees);
    s.melody.root_midi =
        s.melody.root_midi < 24 ? 24 : (s.melody.root_midi > 57 ? 57 : s.melody.root_midi);
    s.drone_hz = clampf(s.drone_hz, 27.5f, 440.0f);
    s.chandas.mix = clampf(s.chandas.mix, 0.0f, 1.0f);
    s.chandas.division = s.chandas.division < (size_t)(CHANDAS_DIVISIONS_LEN - 1)
                             ? s.chandas.division
                             : (size_t)(CHANDAS_DIVISIONS_LEN - 1);
    s.chandas.rate_hz = clampf(s.chandas.rate_hz, 0.1f, 8.0f);
    s.chandas.spread = clampf(s.chandas.spread, 0.0f, 1.0f);
    s.chandas.size = clampf(s.chandas.size, CHANDAS_MIN_SIZE, CHANDAS_MAX_SIZE);
    s.chandas.warp = clampf(s.chandas.warp, 0.0f, 1.0f);
    s.chandas.dimension = clampf(s.chandas.dimension, 0.0f, 1.0f);
    s.chandas.tail = clampf(s.chandas.tail, 0.0f, 1.0f);
    s.tempo_bpm = clampf(s.tempo_bpm, CHANDAS_MIN_BPM, CHANDAS_MAX_BPM);
    s.warmth = clampf(s.warmth, MIN_WARMTH, MAX_WARMTH);
    s.limiter_ceiling_db = clampf(s.limiter_ceiling_db, LIMITER_CEILING_DB_MIN,
                                  LIMITER_CEILING_DB_MAX);
    s.output_gain_db = clampf(s.output_gain_db, OUTPUT_GAIN_DB_MIN,
                              OUTPUT_GAIN_DB_MAX);
    s.attack_s = clampf(s.attack_s, ENV_ATTACK_MIN, ENV_TIME_MAX);
    s.decay_s = clampf(s.decay_s, 0.0f, ENV_TIME_MAX);
    s.sustain = clampf(s.sustain, 0.0f, 1.0f);
    s.release_s = clampf(s.release_s, ENV_RELEASE_MIN, ENV_TIME_MAX);
    if (s.melody.division < 0 || s.melody.division >= CHANDAS_DIVISIONS_LEN)
        s.melody.division = MELODY_DEFAULT_DIVISION;
    s.mods = mod_bank_sanitize(s.mods);
    s.pitch = pitch_seq_sanitize(s.pitch);
    /* one of them plays the notes */
    if (s.pitch.enabled) s.melody.enabled = false;
    return s;
}
