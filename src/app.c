#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio.h"
#include "dsp/dsp.h"
#include "midi.h"
#include "ring.h"

typedef enum { EV_SET_PATCH, EV_SET_VERB, EV_GLIDE_TO } EventKind;

typedef struct {
    EventKind kind;
    union {
        Patch patch;
        VerbParams verb;
        float hz;
    } u;
} Event;

RING_DECLARE(EventRing, Event, 256)

typedef struct {
    Voice voice;
    StereoVerb verb;
    EventRing repl;
    EventRing midi;
} Engine;

static void apply(Engine *e, Event ev) {
    switch (ev.kind) {
    case EV_SET_PATCH:
        voice_set_patch(&e->voice, ev.u.patch);
        verb_configure(&e->verb, &e->voice.patch, &e->voice.compiled);
        break;
    case EV_SET_VERB:
        verb_set_params(&e->verb, ev.u.verb);
        break;
    case EV_GLIDE_TO:
        voice_glide_to_hz(&e->voice, ev.u.hz);
        voice_set_drone_hz(&e->voice, ev.u.hz);
        verb_set_drone_hz(&e->verb, ev.u.hz);
        break;
    }
}

static void render(void *ud, float *data, size_t frames, int channels) {
    Engine *e = ud;
    Event ev;
    while (EventRing_pop(&e->repl, &ev)) apply(e, ev);
    while (EventRing_pop(&e->midi, &ev)) apply(e, ev);
    Frame chunk[64];
    size_t done = 0;
    while (done < frames) {
        size_t n = frames - done;
        if (n > 64) n = 64;
        voice_render_block(&e->voice, chunk, n);
        for (size_t i = 0; i < n; i++) {
            Stereo s = verb_process(&e->verb, &chunk[i]);
            size_t base = (done + i) * (size_t)channels;
            if (channels == 1) {
                data[base] = soft_clip(0.5f * (s.l + s.r));
            } else {
                data[base] = soft_clip(s.l);
                data[base + 1] = soft_clip(s.r);
                for (int c = 2; c < channels; c++) data[base + c] = 0.0f;
            }
        }
        done += n;
    }
}

static void on_midi(void *ud, const uint8_t msg[3]) {
    Engine *e = ud;
    if ((msg[0] & 0xF0) == 0x90 && msg[2] > 0) {
        Event ev = {.kind = EV_GLIDE_TO, .u.hz = midi_to_hz(msg[1])};
        EventRing_push(&e->midi, ev);
    }
}

static const char *mode_name(RatioMode mode) {
    switch (mode) {
    case RATIO_HARMONIC: return "harmonic";
    case RATIO_FIBONACCI: return "fibonacci";
    case RATIO_GOLDEN: return "golden";
    case RATIO_GOLDEN_MIRROR: return "goldenmirror";
    case RATIO_PLASTIC: return "plastic";
    default: return "?";
    }
}

static bool parse_mode(const char *name, RatioMode *out) {
    for (int m = 0; m < RATIO_MODE_COUNT; m++)
        if (strcasecmp(mode_name((RatioMode)m), name) == 0) {
            *out = (RatioMode)m;
            return true;
        }
    return false;
}

static int algorithm_index(const Patch *patch) {
    for (int i = 0; i < 8; i++)
        if (ALGORITHMS[i] == patch->algorithm) return i;
    return 0;
}

static void print_status(const Patch *patch, const VerbParams *verb) {
    Compiled compiled = compile(patch->algorithm);
    char glyphs[NUM_NODES + 1];
    algorithm_to_glyphs(patch->algorithm, glyphs);
    printf("algorithm %d (%s)  mode %s  carriers %d  feedback %.2f  index %.2f"
           "  rip %.2f  master %.2f  glide %.0f ms\n",
           algorithm_index(patch) + 1, glyphs, mode_name(patch->ratio_mode),
           compiled.carrier_count, patch->feedback, patch->index, patch->rip,
           patch->master_level, patch->glide_seconds * 1000.0f);
    printf("room: mix %.2f  ghost %.2f  decay %.1f s  damp %.2f  haunt %.2f\n",
           verb->mix, verb->ghost, verb->decay, verb->damp, verb->haunt);
    for (int i = 0; i < NUM_OPS; i++) {
        printf("  op%d [%s] ratio %.4f  level %.3f  depth %d%s%s\n", i + 1,
               patch->ops[i].enabled ? "on " : "OFF", patch->ops[i].ratio,
               patch->ops[i].level, compiled.depth[i],
               (compiled.carriers >> i & 1) ? "  carrier" : "",
               compiled.feedback_op == i ? "  feedback" : "");
    }
}

static void banner(void) {
    printf("BLOW YOUR PHASE OFF — droning at %g Hz (A2), golden mode, algorithm I\n",
           (double)START_HZ);
    printf("commands:\n");
    printf("  alg <1-8>        switch algorithm; operator tuning stays put\n");
    printf("  mode <name>      load a ratio palette: harmonic | fibonacci | golden | goldenmirror | plastic\n");
    printf("  op <1-5> on|off  operator power switch (off = true reset)\n");
    printf("  ratio <1-5> <x>  set an operator multiplier (%.2f to %.0f)\n",
           (double)OP_RATIO_MIN, (double)OP_RATIO_MAX);
    printf("  oplevel <1-5> <x> set an operator level (0 to 1)\n");
    printf("  note <0-127>     glide to MIDI note   |  hz <freq>  glide to frequency\n");
    printf("  index <0-1>      the INDEX macro: 0 = pure sines, 1 = full madness\n");
    printf("  rip <0-1>        the Rip: carriers phase-modulated by their own inverted past\n");
    printf("  fb <0-1>         feedback   |  level <0-1>  master   |  glide <secs>\n");
    printf("  mix <0-1>        room wet/dry   |  ghost <0-1>  modulator bleed\n");
    printf("  decay <secs>      room decay     |  damp <0-1>   room damping\n");
    printf("  haunt <0-1>      the Ghost Line: phase-inverting cross-feed echoes\n");
    printf("  status           show the current patch and room\n");
    printf("  quit             stop the drone and exit\n");
}

static bool parse_f(const char *s, float lo, float hi, float *out) {
    char *end;
    float x = strtof(s, &end);
    if (end == s || *end != '\0' || !(x >= lo && x <= hi)) return false;
    *out = x;
    return true;
}

int main(void) {
    static Engine engine;
    Engine *e = &engine;

    AudioOut audio;
    if (audio_out_open(&audio) != 0) {
        fprintf(stderr, "no default audio output device\n");
        return 1;
    }
    float sample_rate = (float)audio.rate;
    printf("audio out: default @ %u Hz, %d ch\n", audio.rate, audio.channels);

    Patch patch = patch_init(ALGORITHMS[0], RATIO_GOLDEN);
    voice_init(&e->voice, sample_rate, patch);
    voice_set_freq_hz(&e->voice, START_HZ);
    /* the drone: a note held for as long as the program runs */
    voice_note_on(&e->voice, START_HZ, 1.0f);
    verb_init(&e->verb, sample_rate);
    verb_configure(&e->verb, &e->voice.patch, &e->voice.compiled);

    if (audio_out_start(&audio, render, e) != 0) {
        fprintf(stderr, "could not start audio thread\n");
        return 1;
    }

    MidiIn midi;
    if (midi_in_start(&midi, on_midi, e) == 0)
        printf("MIDI in: %s\n", midi.source_name);
    else
        printf("no MIDI input found — REPL control only\n");
    banner();

    Patch shadow = patch;
    VerbParams shadow_verb = verb_params_default();
    char line[256];
    printf("> ");
    fflush(stdout);
    while (fgets(line, sizeof line, stdin)) {
        char a[64] = "", b[64] = "", c[64] = "";
        int argc = sscanf(line, "%63s %63s %63s", a, b, c);
        bool send_patch = false, send_verb = false;
        float x;
        if (argc <= 0) {
        } else if (strcmp(a, "alg") == 0 && argc >= 2) {
            int k = atoi(b);
            if (k >= 1 && k <= 8) {
                shadow.algorithm = ALGORITHMS[k - 1];
                send_patch = true;
            } else
                printf("alg wants 1-8\n");
        } else if (strcmp(a, "mode") == 0 && argc >= 2) {
            RatioMode m;
            if (parse_mode(b, &m)) {
                patch_apply_ratio_mode(&shadow, m);
                send_patch = true;
            } else
                printf("modes: harmonic fibonacci golden goldenmirror plastic\n");
        } else if (strcmp(a, "op") == 0 && argc >= 3) {
            int k = atoi(b);
            if (k >= 1 && k <= NUM_OPS
                && (strcmp(c, "on") == 0 || strcmp(c, "off") == 0)) {
                shadow.ops[k - 1].enabled = strcmp(c, "on") == 0;
                send_patch = true;
            } else
                printf("usage: op <1-%d> on|off\n", NUM_OPS);
        } else if (strcmp(a, "ratio") == 0 && argc >= 3) {
            int k = atoi(b);
            if (k >= 1 && k <= NUM_OPS && parse_f(c, OP_RATIO_MIN,
                                                   OP_RATIO_MAX, &x)) {
                shadow.ops[k - 1].ratio = x;
                send_patch = true;
            } else
                printf("usage: ratio <1-%d> <%.2f-%.0f>\n", NUM_OPS,
                       (double)OP_RATIO_MIN, (double)OP_RATIO_MAX);
        } else if (strcmp(a, "oplevel") == 0 && argc >= 3) {
            int k = atoi(b);
            if (k >= 1 && k <= NUM_OPS && parse_f(c, 0.0f, 1.0f, &x)) {
                shadow.ops[k - 1].level = x;
                send_patch = true;
            } else
                printf("usage: oplevel <1-%d> <0-1>\n", NUM_OPS);
        } else if (strcmp(a, "note") == 0 && argc >= 2) {
            int n = atoi(b);
            if (n >= 0 && n < 128) {
                Event ev = {.kind = EV_GLIDE_TO, .u.hz = midi_to_hz((uint8_t)n)};
                EventRing_push(&e->repl, ev);
            } else
                printf("note wants 0-127\n");
        } else if (strcmp(a, "hz") == 0 && argc >= 2) {
            if (parse_f(b, 1e-6f, 1e9f, &x)) {
                Event ev = {.kind = EV_GLIDE_TO, .u.hz = x};
                EventRing_push(&e->repl, ev);
            } else
                printf("hz wants a positive frequency\n");
        } else if (strcmp(a, "fb") == 0 && argc >= 2) {
            if (parse_f(b, 0.0f, 1.0f, &shadow.feedback)) send_patch = true;
            else printf("fb wants 0-1\n");
        } else if (strcmp(a, "index") == 0 && argc >= 2) {
            if (parse_f(b, 0.0f, 1.0f, &shadow.index)) send_patch = true;
            else printf("index wants 0-1\n");
        } else if (strcmp(a, "rip") == 0 && argc >= 2) {
            if (parse_f(b, 0.0f, 1.0f, &shadow.rip)) send_patch = true;
            else printf("rip wants 0-1\n");
        } else if (strcmp(a, "mix") == 0 && argc >= 2) {
            if (parse_f(b, 0.0f, 1.0f, &shadow_verb.mix)) send_verb = true;
            else printf("mix wants 0-1\n");
        } else if (strcmp(a, "ghost") == 0 && argc >= 2) {
            if (parse_f(b, 0.0f, 1.0f, &shadow_verb.ghost)) send_verb = true;
            else printf("ghost wants 0-1\n");
        } else if (strcmp(a, "decay") == 0 && argc >= 2) {
            if (parse_f(b, 0.05f, 60.0f, &shadow_verb.decay)) send_verb = true;
            else printf("decay wants seconds (0.05-60)\n");
        } else if (strcmp(a, "damp") == 0 && argc >= 2) {
            if (parse_f(b, 0.0f, 0.99f, &shadow_verb.damp)) send_verb = true;
            else printf("damp wants 0-0.99\n");
        } else if (strcmp(a, "haunt") == 0 && argc >= 2) {
            if (parse_f(b, 0.0f, 1.0f, &shadow_verb.haunt)) send_verb = true;
            else printf("haunt wants 0-1\n");
        } else if (strcmp(a, "level") == 0 && argc >= 2) {
            if (parse_f(b, 0.0f, 1.0f, &shadow.master_level)) send_patch = true;
            else printf("level wants 0-1\n");
        } else if (strcmp(a, "glide") == 0 && argc >= 2) {
            if (parse_f(b, 0.0f, 30.0f, &shadow.glide_seconds)) send_patch = true;
            else printf("glide wants seconds (0-30)\n");
        } else if (strcmp(a, "status") == 0) {
            print_status(&shadow, &shadow_verb);
        } else if (strcmp(a, "quit") == 0 || strcmp(a, "exit") == 0) {
            break;
        } else {
            printf("unknown command — see the banner above\n");
        }
        if (send_patch) {
            Event ev = {.kind = EV_SET_PATCH, .u.patch = shadow};
            if (!EventRing_push(&e->repl, ev))
                fprintf(stderr, "control ring full — command dropped\n");
        }
        if (send_verb) {
            Event ev = {.kind = EV_SET_VERB, .u.verb = shadow_verb};
            if (!EventRing_push(&e->repl, ev))
                fprintf(stderr, "control ring full — command dropped\n");
        }
        printf("> ");
        fflush(stdout);
    }

    midi_in_stop(&midi);
    audio_out_stop(&audio);
    verb_free(&e->verb);
    voice_free(&e->voice);
    printf("drone stopped.\n");
    return 0;
}
