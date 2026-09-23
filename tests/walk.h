/* Test walks over a rendered block. The audio path writes the block and
   reads it in place; these only exist so a check can look at each frame. */
#ifndef BYPO_TEST_WALK_H
#define BYPO_TEST_WALK_H

#include "../src/dsp/dsp.h"

static inline void voice_skip(Voice *v, size_t count) {
    Frame buf[64];
    while (count) {
        size_t n = count < 64 ? count : 64;
        voice_render_block(v, buf, n);
        count -= n;
    }
}

static inline void pair_skip(VoicePair *p, size_t count) {
    Frame buf[128];
    while (count) {
        size_t n = count < 128 ? count : 128;
        voice_pair_render_block(p, buf, n);
        count -= n;
    }
}

static inline void bank_skip(VoiceBank *b, size_t count) {
    Frame buf[128];
    while (count) {
        size_t n = count < 128 ? count : 128;
        voice_bank_render_block(b, buf, n);
        count -= n;
    }
}

static inline void voice_each(Voice *v, size_t count,
                              void (*fn)(void *, const Frame *), void *ud) {
    Frame buf[64];
    while (count) {
        size_t n = count < 64 ? count : 64;
        voice_render_block(v, buf, n);
        for (size_t i = 0; i < n; i++) fn(ud, &buf[i]);
        count -= n;
    }
}

static inline void pair_each(VoicePair *p, size_t count,
                             void (*fn)(void *, const Frame *), void *ud) {
    Frame buf[128];
    while (count) {
        size_t n = count < 128 ? count : 128;
        voice_pair_render_block(p, buf, n);
        for (size_t i = 0; i < n; i++) fn(ud, &buf[i]);
        count -= n;
    }
}

static inline void bank_each(VoiceBank *b, size_t count,
                             void (*fn)(void *, const Frame *), void *ud) {
    Frame buf[128];
    while (count) {
        size_t n = count < 128 ? count : 128;
        voice_bank_render_block(b, buf, n);
        for (size_t i = 0; i < n; i++) fn(ud, &buf[i]);
        count -= n;
    }
}

#endif
