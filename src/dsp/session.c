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
    s.patch.field = clampf(s.patch.field, 0.0f, 1.0f);
    s.patch.curve = clampf(s.patch.curve, 0.0f, 1.0f);
    for (int i = 0; i < NUM_OPS; i++) {
        s.patch.ops[i].level = clampf(s.patch.ops[i].level, 0.0f, 1.0f);
        s.patch.ops[i].ratio = clampf(s.patch.ops[i].ratio, 0.01f, 64.0f);
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
    return s;
}
