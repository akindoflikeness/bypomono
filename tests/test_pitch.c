#include "../src/dsp/dsp.h"
#include "walk.h"
#include "test.h"

#define SR 48000.0f
#define NOTE_HZ 220.0f

/* Algorithm V is the all-parallel case. Its carrier oscillators must retain
   their individual palette ratios rather than being collapsed to five 1x
   copies in the name of a virtual fundamental. */
static void additive_carriers_keep_their_own_frequencies(void) {
    Patch patch = patch_init(ALGORITHMS[4], RATIO_HARMONIC); /* PPPP */
    patch.index = 0.0f;
    patch.feedback = 0.0f;
    patch.rip = 0.0f;
    Voice v;
    voice_init(&v, SR, patch);
    voice_set_freq_hz(&v, NOTE_HZ);

    float ignore;
    voice_render(&v, &ignore, 1);
    for (int op = 0; op < NUM_OPS; op++) {
        float want = NOTE_HZ * patch.ops[op].ratio / SR;
        CHECK_NEAR(voice_op_phase(&v, op), want, 1e-6f,
                   "additive op %d phase %g vs %g", op + 1,
                   (double)voice_op_phase(&v, op), (double)want);
    }

    Frame frame;
    voice_render_block(&v, &frame, 1);
    for (int op = 1; op < NUM_OPS; op++)
        CHECK(fabsf(frame.ops[op] - frame.ops[0]) > 1e-4f,
              "additive op %d collapsed onto op 1", op + 1);
    voice_free(&v);
}

/* Carrier level is a literal gain into the audible mix. Topology must not
   quietly divide that gain away merely because it exposes more carriers. */
static void additive_carriers_are_summed_at_the_output(void) {
    Patch patch = patch_init(ALGORITHMS[4], RATIO_HARMONIC); /* PPPP */
    patch.index = 0.0f;
    patch.feedback = 0.0f;
    patch.rip = 0.0f;
    Voice v;
    voice_init(&v, SR, patch);
    voice_set_freq_hz(&v, NOTE_HZ);
    voice_note_on(&v, NOTE_HZ, 1.0f);

    voice_skip(&v, 256);
    Frame frame;
    voice_render_block(&v, &frame, 1);
    float carriers = 0.0f;
    for (int op = 0; op < NUM_OPS; op++) carriers += frame.ops[op];
    CHECK_NEAR(frame.mix, carriers * frame.master, 1e-5f,
               "mix %g is not the carrier sum %g", (double)frame.mix,
               (double)(carriers * frame.master));
    voice_free(&v);
}

void test_pitch(void) {
    additive_carriers_keep_their_own_frequencies();
    additive_carriers_are_summed_at_the_output();
}
