#ifndef BYPO_DSP_H
#define BYPO_DSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------- shared math ---------- */

#define PHI 1.618034f
#define PLASTIC 1.324718f
#define TAU_F 6.2831853071795864769f
#define PI_F 3.14159265358979323846f

/* base^n by successive squaring; reciprocal for negative n */
float powi_f(float base, int n);
/* x - floor(x): wraps a phase into [0, 1) */
static inline float fract_pos(float x);

typedef struct { float l, r; } Stereo;

/* ---------- phase ---------- */

#define PHASE_PER_PASS (PI_F / 5.0f)

float allpass_phase(float a, float w);
float allpass_coeff_for(float hz, float sample_rate, float target);

typedef struct { float a, x1, y1; } PhaseRotator;
void phase_rotator_init(PhaseRotator *p, float a);
float phase_rotator_process(PhaseRotator *p, float x);
void phase_rotator_clear(PhaseRotator *p);

/* ---------- master ---------- */

#define SOFT_CLIP_KNEE 0.7f
float soft_clip(float x);
float soft_clip_to(float x, float ceiling);

/* ---------- output limiter ---------- */

/* The final output guard is deliberately separate from the musical tape and
   feedback saturators. It looks one millisecond ahead, links left/right gain,
   and measures four interpolated positions per sample before applying a hard
   -1 dBTP ceiling. Its fixed delay keeps bypass timing stable. */
#define LIMITER_CEILING_DB_DEFAULT (-1.0f)
#define LIMITER_CEILING_DB_MIN (-12.0f)
#define LIMITER_CEILING_DB_MAX (-0.1f)
#define LIMITER_LOOKAHEAD_S 0.001f
#define LIMITER_RELEASE_S 0.080f

typedef struct {
    Stereo *delay;
    float *peaks;
    size_t len, write;
    float history_l[4], history_r[4];
    float gain, release_k, ceiling, reduction_db;
    bool enabled;
} Limiter;

void limiter_init(Limiter *l, float sample_rate);
void limiter_free(Limiter *l);
void limiter_clear(Limiter *l);
void limiter_set(Limiter *l, bool enabled, float ceiling_db);
Stereo limiter_process(Limiter *l, Stereo x);
float limiter_reduction_db(const Limiter *l);

/* ---------- gate ---------- */

/* the house glide: every switch and every coefficient travels over this,
   because nothing in the engine may arrive instantly */
#define GATE_GLIDE_S 0.013f
#define GATE_FLOOR 1e-6f


/* ---------- algorithm ---------- */

#define NUM_OPS 5
#define NUM_NODES 4
#define ALGORITHM_SPACE 81

/* Direct-operator tuning contract shared by the UI, plug-in, and session
   loader. These bound the control surface only: neither note pitch nor an
   algorithm is allowed to derive or replace an operator's stored ratio. */
#define OP_RATIO_MIN 0.01f
#define OP_RATIO_MAX 64.0f

typedef enum { COMBINE_SERIES = 0, COMBINE_PARALLEL = 1, COMBINE_FEEDBACK = 2 } Combine;
typedef uint8_t AlgorithmId; /* base-3 trits, one per node; compare with == */

char combine_glyph(Combine c);
Combine combine_next(Combine c);

Combine algorithm_node(AlgorithmId id, int k);
AlgorithmId algorithm_with_node(AlgorithmId id, int k, Combine choice);
int algorithm_distance(AlgorithmId a, AlgorithmId b);
void algorithm_to_glyphs(AlgorithmId id, char out[NUM_NODES + 1]); /* NUL-terminated */
bool algorithm_from_glyphs(const char *text, AlgorithmId *out);    /* text must be 4 chars */
AlgorithmId algorithm_from_legacy_bits(uint8_t bits);

extern const AlgorithmId ALGORITHMS[8];

typedef struct {
    uint8_t modulators[NUM_OPS]; /* bitmask of ops modulating op i */
    uint8_t carriers;            /* bitmask */
    int eval_order[NUM_OPS];
    uint8_t depth[NUM_OPS];
    int feedback_op;
    uint8_t carrier_count;
} Compiled;

Compiled compile(AlgorithmId id);

/* ---------- patch ---------- */

#define HEADROOM_DB (-14.0f)
#define POLY_MAX 4
#define UNISON_MAX 2
#define UNISON_DETUNE_MAX 50.0f
#define UNISON_DETUNE_DEFAULT 10.0f

float midi_to_hz(uint8_t note);
float index_response(uint8_t depth, float index);
float master_gain(float position);

typedef enum {
    RATIO_HARMONIC = 0,
    RATIO_FIBONACCI,
    RATIO_GOLDEN, /* default */
    RATIO_GOLDEN_MIRROR,
    RATIO_PLASTIC,
    RATIO_MODE_COUNT
} RatioMode;

float ratio_mode_ratio(RatioMode mode, int op);

typedef struct {
    bool enabled;
    float ratio;
    float detune_cents;
    float level;
} OpParams;

typedef struct {
    AlgorithmId algorithm;
    RatioMode ratio_mode;
    OpParams ops[NUM_OPS];
    float feedback;
    float index;
    float rip;
    float master_level;
    float glide_seconds;
    uint8_t voices;      /* notes at once: 1 = mono, up to POLY_MAX */
    uint8_t unison;      /* voices per note: 1 or UNISON_MAX */
    float unison_detune; /* cents between the two unison voices */
} Patch;

Patch patch_init(AlgorithmId algorithm, RatioMode ratio_mode);
/* Loads one of the named ratio palettes into the operators. This is an
   explicit tuning action: changing an algorithm never calls it. */
void patch_apply_ratio_mode(Patch *patch, RatioMode ratio_mode);

/* ---------- envelope ---------- */

typedef struct {
    float attack_s, decay_s, release_s;
    float sustain;
} EnvParams;
EnvParams env_params_default(void); /* 0.008, 2.0, 2.0, 1.0 */

#define ENV_TIME_MAX 8.0f
#define ENV_RELEASE_MIN 0.05f
/* the shortest attack that still arrives instead of ticking: a note landing
   on a voice whose tail has nearly gone would otherwise slam from silence to
   full level inside a couple of samples */
#define ENV_ATTACK_MIN 0.002f

#define ENV_FLOOR 1e-4f
typedef enum { ENV_IDLE, ENV_HELD, ENV_RELEASED } EnvStage;

typedef struct {
    EnvStage stage;
    float t, from, level, peak, sample_rate;
    EnvParams seen; /* the settings the clock below was measured against */
} Envelope;

void envelope_init(Envelope *e, float sample_rate);
void envelope_note_on(Envelope *e);
void envelope_note_off(Envelope *e);
bool envelope_active(const Envelope *e);
float envelope_level(const Envelope *e);
float envelope_tick(Envelope *e, const EnvParams *p);

/* ---------- state ---------- */

typedef struct {
    Patch patch;
    EnvParams adsr; /* every voice's amplitude */
} State;

State state_new(Patch patch);
bool state_is_structural_change(const State *a, const State *b);

/* ---------- voice ---------- */

typedef struct {
    float ops[NUM_OPS];
    float mix;
    float master;
    float base_hz;
    float side; /* dry mix difference: left gets mix + side, right mix - side */
} Frame;

typedef struct {
    float *buf;
    size_t len;
    size_t write;
    size_t delay;
    PhaseRotator rot;
    float rot_hz;  /* pitch the rotator is currently solved for */
    float rot_to;  /* coefficient it is travelling toward */
    float rot_k;
    float lp;
    float fb;
    float in_gain, in_k; /* the input fades in after a clear */
} RipLine;

typedef struct {
    float sample_rate;
    Patch patch;
    Compiled compiled;
    float phase[NUM_OPS];
    float out[NUM_OPS];
    float fb_hist[2];
    float gain[NUM_OPS];
    bool pending_reset[NUM_OPS];
    float freq, target_freq;
    /* A reassigned poly voice gets this temporary minimum glide so an
       arbitrary manual ratio set cannot create a hard steal seam. */
    float steal_glide_seconds;
    float master, index;
    float fb_smooth; /* glided, so modulating fb cannot zipper */
    float level_s[NUM_OPS]; /* op levels are gains straight to the mix */
    RipLine rip_line;
    float rip_sig, rip_smooth;
    float bend, bend_to;
    float detune, detune_to; /* frequency ratio from the unison spread */
    /* Velocity is a gain outside the normalized envelope. It is smoothed on
       a sounding voice so a different retrigger velocity cannot step the VCA. */
    float velocity, velocity_to;
    /* index^exponent, recomputed when index has actually moved */
    float pow_index, fb_pow, index_pow[NUM_OPS];
    float ratio_s[NUM_OPS]; /* operator ratios glide; a jump is a chirp, not a click */
    EnvParams adsr;
    Envelope env;
} Voice;

void voice_init(Voice *v, float sample_rate, Patch patch); /* allocates rip buffer */
void voice_free(Voice *v);
void voice_note_off(Voice *v);
void voice_set_freq_hz(Voice *v, float hz);
void voice_set_drone_hz(Voice *v, float hz); /* retunes the rip rotator */
void voice_set_adsr(Voice *v, EnvParams adsr);
void voice_note_on(Voice *v, float hz, float velocity);
/* Like note_on, but a held polyphonic voice is being reassigned. It preserves
   the user's normal glide setting while imposing a brief click-safe minimum. */
void voice_note_steal(Voice *v, float hz, float velocity);
bool voice_note_sounding(const Voice *v);
void voice_set_bend_semitones(Voice *v, float semitones);
void voice_set_detune_cents(Voice *v, float cents);
/* clears the rip line's memory before a silent voice takes a new note */
void voice_wake(Voice *v);
float voice_target_hz(const Voice *v);
void voice_glide_to_hz(Voice *v, float hz);
void voice_drone_to_hz(Voice *v, float hz); /* glide, gesture only if settled */
void voice_set_op_enabled(Voice *v, int op, bool on);
void voice_set_patch(Voice *v, Patch patch);
/* park the gliding ratios on the patch, for a voice that is not sounding
   or has just passed through silence */
void voice_snap_ratios(Voice *v);
void voice_set_algorithm(Voice *v, AlgorithmId algorithm);
float voice_op_phase(const Voice *v, int op);
/* writes `count` frames into out */
void voice_render_block(Voice *v, Frame *out, size_t count);
void voice_render(Voice *v, float *buf, size_t len);

/* ---------- pair ---------- */

/* A shape change (algorithm, ratio palette) cannot glide. The note dips
   through silence and the new shape lands while nothing is coming out. */
#define STRUCT_DIP_SECONDS 0.005f

typedef struct {
    Voice voice;
    float sample_rate;
    float dip;     /* 1 = full level, 0 = silent */
    int dip_len;   /* samples in one 5 ms leg */
    int dip_left;  /* samples left in the current leg */
    int dip_dir;   /* -1 down, +1 up, 0 parked at full */
    bool armed;    /* a shape change is waiting to land at silence */
    State pending;
    Compiled pending_compiled; /* what the room follows until the dip lands */
} VoicePair;

void voice_pair_init(VoicePair *p, float sample_rate, Patch patch);
void voice_pair_free(VoicePair *p);
bool voice_pair_crossing(const VoicePair *p);
State voice_pair_state(const VoicePair *p);
void voice_pair_set_state(VoicePair *p, State next);
void voice_pair_set_patch(VoicePair *p, Patch patch);
const Patch *voice_pair_patch(const VoicePair *p);
const Compiled *voice_pair_compiled(const VoicePair *p);
void voice_pair_set_op_enabled(VoicePair *p, int op, bool on);
void voice_pair_set_freq_hz(VoicePair *p, float hz);
void voice_pair_set_drone_hz(VoicePair *p, float hz);
void voice_pair_glide_to_hz(VoicePair *p, float hz);
void voice_pair_drone_to_hz(VoicePair *p, float hz);
void voice_pair_note_on(VoicePair *p, float hz, float velocity);
void voice_pair_note_steal(VoicePair *p, float hz, float velocity);
bool voice_pair_note_sounding(const VoicePair *p);
const EnvParams *voice_pair_adsr(const VoicePair *p);
void voice_pair_set_bend_semitones(VoicePair *p, float semitones);
void voice_pair_note_off(VoicePair *p);
float voice_pair_target_hz(const VoicePair *p);
void voice_pair_render_block(VoicePair *p, Frame *out, size_t count);
void voice_pair_set_adsr_now(VoicePair *p, EnvParams adsr); /* no dip */
void voice_pair_set_detune_cents(VoicePair *p, float cents);
void voice_pair_wake(VoicePair *p);
/* the envelope has finished */
bool voice_pair_silent(const VoicePair *p);

/* ---------- bank ---------- */

#define BANK_PAIRS (POLY_MAX * UNISON_MAX)
#define DRONE_KEY (-2)
#define BANK_CHUNK 128
#define UNISON_WIDTH 0.5f

/* Up to POLY_MAX notes, each played by UNISON_MAX pairs.
   Slot = note * UNISON_MAX + copy. The drone is a held gate on note 0: it
   holds that note's envelope at sustain until it is let go. In poly, keys
   play the other notes; in mono a key borrows note 0 and hands it back. */
typedef struct {
    VoicePair pairs[BANK_PAIRS];
    float gain[BANK_PAIRS];
    int key[POLY_MAX]; /* -1 = no key (sequencer), DRONE_KEY = the drone */
    bool held[POLY_MAX];
    uint32_t stamp[POLY_MAX];
    uint32_t clock;
    int newest;
    int poly; /* note slots reachable under the current patch */
    bool drone;     /* the drone's gate is held */
    float drone_hz; /* where note 0 returns when a key lets go of it */
    float spread, step;
    Frame scratch[BANK_PAIRS][BANK_CHUNK];
} VoiceBank;

void voice_bank_init(VoiceBank *b, float sample_rate, Patch patch);
void voice_bank_free(VoiceBank *b);
bool voice_bank_crossing(const VoiceBank *b);
State voice_bank_state(const VoiceBank *b);
void voice_bank_set_state(VoiceBank *b, State next);
void voice_bank_set_patch(VoiceBank *b, Patch patch);
void voice_bank_set_adsr_now(VoiceBank *b, EnvParams adsr);
const Patch *voice_bank_patch(const VoiceBank *b);
const Compiled *voice_bank_compiled(const VoiceBank *b);
const EnvParams *voice_bank_adsr(const VoiceBank *b);
int voice_bank_poly(const VoiceBank *b);
void voice_bank_set_freq_hz(VoiceBank *b, float hz);
void voice_bank_set_drone_hz(VoiceBank *b, float hz);
void voice_bank_glide_to_hz(VoiceBank *b, float hz); /* the drone, note 0 */
void voice_bank_drone_to_hz(VoiceBank *b, float hz);  /* the drone hz control */
/* moves the newest note's pitch without touching its envelope */
void voice_bank_glide_newest_to_hz(VoiceBank *b, float hz);
/* holds or lets go of the drone's gate on note 0 */
void voice_bank_set_drone(VoiceBank *b, bool held);
void voice_bank_note_on(VoiceBank *b, int key, float hz, float velocity);
/* releases the note on `key`. key < 0 is the wildcard and lets every note go.
   The drone's gate stays held either way. */
void voice_bank_note_off(VoiceBank *b, int key);
/* the slot whose key is exactly `key`, and no other. -1 is the sequencer's
   key, not the wildcard. */
void voice_bank_note_off_exact(VoiceBank *b, int key);
void voice_bank_note_off_all(VoiceBank *b);
bool voice_bank_note_sounding(const VoiceBank *b);
void voice_bank_set_bend_semitones(VoiceBank *b, float semitones);
float voice_bank_target_hz(const VoiceBank *b); /* the newest note */
const Envelope *voice_bank_newest_env(const VoiceBank *b);
/* target hz of each held note (the drone counts as held); returns count */
int voice_bank_held_hz(const VoiceBank *b, float out[POLY_MAX]);
/* writes `count` mixed frames into out. The bank renders in BANK_CHUNK
   pieces; out must hold every frame. */
void voice_bank_render_block(VoiceBank *b, Frame *out, size_t count);

/* ---------- shared filter/delay primitives ---------- */

#define DC_BLOCK_R 0.9995f
typedef struct { float x1, y1; } DcBlock;
float dc_block_process(DcBlock *d, float x); /* r = DC_BLOCK_R */
void dc_block_clear(DcBlock *d);

typedef struct { float ic1, ic2; } Svf;
float svf_process(Svf *s, float x, float g, float k);    /* lowpass */
float svf_process_hp(Svf *s, float x, float g, float k); /* highpass */

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } Biquad;
void biquad_highpass(Biquad *q, float hz, float sr); /* Q = 1/sqrt(2) */
void biquad_allpass(Biquad *q, float hz, float sr);
float biquad_process(Biquad *q, float x);
void biquad_clear(Biquad *q);

typedef struct {
    float *buf;
    size_t len;
    size_t w;
} Delay;
void delay_init(Delay *d, size_t max);   /* len = max(max,4), zeroed */
void delay_free(Delay *d);
void delay_write(Delay *d, float x);
float delay_read(const Delay *d, size_t back);
float delay_read_frac(const Delay *d, float back);
void delay_clear(Delay *d);

/* ---------- chamber ---------- */

#define CHAMBER_N 8
#define CHAMBER_DAMP 0.27f
#define CHAMBER_WIDTH 1.47f
#define CHAMBER_MOD_CENTS 9.3f
#define CHAMBER_PREDELAY_MS 130.0f
#define CHAMBER_HPF_HZ 38.0f

void hadamard8(float s[CHAMBER_N]);

typedef struct {
    float sr;
    Delay pre;
    Biquad hpf_l, hpf_r;
    size_t pre_samples;
    Delay ap[4];
    Delay lines[CHAMBER_N];
    float lp[CHAMBER_N];
    float dc_x1[CHAMBER_N], dc_y1[CHAMBER_N];
    float dc_r;
    float mod_ph[CHAMBER_N];
    float len[CHAMBER_N], g[CHAMBER_N];
    float len_to[CHAMBER_N], g_to[CHAMBER_N];
    float norm[CHAMBER_N], norm_g[CHAMBER_N]; /* sqrt(1-g^2), held while g is still */
    float glide, damp_a, mod_samples;
    float mix, mix_to;
} Chamber;

void chamber_init(Chamber *c, float sr);
void chamber_free(Chamber *c);
float chamber_tail_seconds(float tail);
void chamber_set(Chamber *c, float dimension, float tail);
void chamber_clear(Chamber *c);
Stereo chamber_process(Chamber *c, Stereo dry);

/* ---------- melody ---------- */

#define NUM_SCALES 8
#define NUM_TUNINGS 5
#define MELODY_PITCH_CAP_HZ 4186.0f

typedef enum {
    SCALE_PHRYGIAN = 0,
    SCALE_PHRYGIAN_DOMINANT,
    SCALE_NATURAL_MINOR,
    SCALE_HARMONIC_MINOR,
    SCALE_NEAPOLITAN_MINOR,
    SCALE_BYZANTINE,
    SCALE_FIBONACCI,
    SCALE_PHYLLOTAXIS
} Scale;

const uint8_t *scale_intervals(Scale s, size_t *count);
const char *scale_name(Scale s);

typedef enum {
    TUNING_SCALE = 0,
    TUNING_FIBONACCI_HZ,
    TUNING_GOLDEN_POWERS,
    TUNING_PLASTIC_POWERS,
    TUNING_GOLDEN_WALK
} Tuning;

const char *tuning_name(Tuning t);

typedef enum { QUANT_TWELVE_TET, QUANT_GRID, QUANT_FREE } QuantKind;
typedef struct {
    QuantKind kind;
    const char *symbol; /* "" for twelve-tet */
} Quantization;

Quantization tuning_quantization(Tuning t);
bool quantization_is_quantized(Quantization q);

typedef enum { HOLD_GOLDEN_WEYL = 0, HOLD_XORSHIFT } HoldSource;

typedef struct {
    bool enabled;
    Tuning tuning;
    Scale scale;
    uint8_t root_midi;
    uint8_t range_degrees;
    float rate_hz;
    HoldSource source;
    bool sync;       /* notes on the clock instead of at rate_hz */
    int8_t division; /* CHANDAS_DIVISIONS index, one note's length */
} MelodyParams;
MelodyParams melody_params_default(void);
#define MELODY_DEFAULT_DIVISION 18 /* 1/8 */

typedef struct {
    MelodyParams params;
    float sample_rate;
    float bpm;
    size_t countdown;
    float weyl;
    uint32_t rng;
    float walk_hz;
} Melody;

void melody_init(Melody *m, float sample_rate, MelodyParams params);
void melody_set_params(Melody *m, MelodyParams params);
void melody_set_tempo(Melody *m, float bpm);
bool melody_samples_until_fire(const Melody *m, size_t *out);
void melody_advance(Melody *m, size_t samples);
float melody_fire(Melody *m);

/* ---------- pitch sequencer ---------- */

#define PITCH_STEPS 16
#define PITCH_RANGE_ST 24.0f /* each step reaches this far above or below root */
#define PITCH_DEFAULT_DIVISION 21 /* 1/16 */
#define PITCH_GATE_LEN_MIN 0.05f

/* A note sequencer on the clock: each step has a pitch, a gate and a
   velocity. A gated step plays a note that lasts gate_len of the step. */
typedef struct {
    bool enabled;
    int8_t division;   /* CHANDAS_DIVISIONS index, one step's length */
    uint8_t length;    /* steps played, 1..16 */
    uint8_t root_midi;
    bool snap;         /* round each step to whole semitones */
    float gate_len;    /* fraction of a step the note is held */
    float pitch[PITCH_STEPS]; /* semitones from the root */
    bool gate[PITCH_STEPS];
    float velocity[PITCH_STEPS]; /* 0..1 */
} PitchSeqParams;
PitchSeqParams pitch_seq_params_default(void); /* off, every step gated at root */
PitchSeqParams pitch_seq_sanitize(PitchSeqParams p);
/* the semitones a step plays, snapped when snap is on */
float pitch_seq_semitones(const PitchSeqParams *p, int step);
float pitch_seq_step_seconds(const PitchSeqParams *p, float bpm);

/* MOVE is a gate-off step: the pitch follows the line, the envelope is left
   alone */
typedef enum { PITCH_EV_NONE, PITCH_EV_ON, PITCH_EV_OFF, PITCH_EV_MOVE } PitchEventKind;
typedef struct {
    PitchEventKind kind;
    float hz, velocity; /* ON; MOVE uses hz */
} PitchEvent;

typedef struct {
    PitchSeqParams params;
    float sample_rate, bpm;
    int step;             /* the step playing now, -1 before the first */
    size_t until_step;    /* samples until the next step starts */
    size_t until_off;     /* samples until the held note ends */
    bool held;
} PitchSeq;

void pitch_seq_init(PitchSeq *s, float sample_rate);
void pitch_seq_set_params(PitchSeq *s, PitchSeqParams p);
void pitch_seq_set_tempo(PitchSeq *s, float bpm);
/* samples until something is due; false while it is off */
bool pitch_seq_samples_until(const PitchSeq *s, size_t *out);
/* the next thing due now: a note ending comes before a step starting. Call
   until it returns PITCH_EV_NONE. */
PitchEvent pitch_seq_fire(PitchSeq *s);
void pitch_seq_advance(PitchSeq *s, size_t samples);

/* ---------- reverb ---------- */

typedef struct {
    float mix, ghost, decay, damp, haunt;
} VerbParams;
VerbParams verb_params_default(void); /* 0.35, 0.4, 4.0, 0.4, 0.0 */

#define VERB_Q_BASE 0.70710678f          /* 1/sqrt(2) */
#define VERB_Q_KNEE (1.0f / (PHI * PHI))
#define VERB_ROOM_HP_K (PHI * PHI)
#define VERB_ROOM_HP_MIN_HZ 12.0f
#define VERB_ROOM_HP_MAX_HZ 100.0f
#define VERB_MOD_DEPTH_SAMPLES 9.0f

float verb_room_hp_hz(float base_hz);
float verb_damp_q(float damp);

typedef struct {
    float *buf;
    size_t len;
    size_t write;
    size_t delay;
    float delay_f, delay_to; /* samples; the read travels between lengths */
    float delay_seconds;
    float in_ref; /* 1/sqrt(1 - fb^2) at the default decay */
    float fb, fb_target, lp;
    float lfo_phase, lfo_inc;
    float in_gain, in_gain_fb; /* sqrt(1-fb^2)*in_ref, held while fb is still */
} Comb;

typedef struct {
    float *buf;
    size_t len;
    size_t write;
    size_t delay;
    float delay_f, delay_to;
    PhaseRotator rot;
    float rot_to;
    float fb;
} GhostLine;

typedef struct {
    float *buf;
    size_t len;
    size_t write;
    size_t delay;
} Allpass;

typedef struct {
    float sample_rate;
    VerbParams params;
    float mix_s, ghost_s, haunt_s, damp_s;
    float send[NUM_OPS], send_to[NUM_OPS];
    float carrier[NUM_OPS], carrier_to[NUM_OPS]; /* 1 = carrier, ramped */
    bool is_carrier[NUM_OPS];
    Comb combs_l[NUM_OPS], combs_r[NUM_OPS];
    Allpass ap_l[2], ap_r[2];
    GhostLine ghosts[NUM_OPS];
    DcBlock dc[NUM_OPS];
    DcBlock wet_dc_l, wet_dc_r;
    Svf room_hp_l, room_hp_r;
    float room_hp_g, room_hp_for_hz;
    float damp_for, wet_g, wet_k; /* the damp filter, held while damp is still */
    float ghost_hz; /* pitch the ghost rotators are currently solved for */
    bool configured; /* the first configure lands outright, later ones travel */
    Svf svf_l[2], svf_r[2];
} StereoVerb;

void verb_init(StereoVerb *v, float sample_rate);
void verb_free(StereoVerb *v);
void verb_configure(StereoVerb *v, const Patch *patch, const Compiled *compiled);
void verb_set_params(StereoVerb *v, VerbParams params);
void verb_set_drone_hz(StereoVerb *v, float hz); /* retunes the ghost rotators */
VerbParams verb_params(const StereoVerb *v);
Stereo verb_process(StereoVerb *v, const Frame *frame);

/* ---------- tape ---------- */

#define MIN_WARMTH 0.0f
#define MAX_WARMTH 1.0f

#define COMP_THRESHOLD_DB (-12.0f)
#define COMP_RATIO 2.0f
#define COMP_NOMINAL_DBFS (-18.0f)
#define COMP_KNEE_DB 8.0f
#define COMP_ATTACK_MS 15.0f
#define COMP_RELEASE_FAST_MS 2.0f
#define COMP_RELEASE_SLOW_MS 550.0f
#define COMP_MAX_REDUCTION_DB 12.0f

#define TETHER_HZ 3000.0f
#define TETHER_AIR_HZ 9000.0f
#define TETHER_THRESHOLD_DB (-34.0f)
#define TETHER_LIFT_DB 6.0f
#define TETHER_RATIO 4.0f
#define TETHER_KNEE_DB 10.0f
#define TETHER_AP_LO_HZ 1200.0f
#define TETHER_AP_HI_HZ 4000.0f

float tape_saturate(float x); /* == soft_clip */
float tether_lift_db(float env, float lift);

typedef struct { float lp, k, gain; } Shelf;
typedef struct { float lp, k; } Top;
typedef struct { Biquad a, b; } Hp4;
typedef struct {
    Hp4 l, r;
    float env;
} TetherBand;

typedef struct {
    float sample_rate;
    float warmth, warmth_s;
    float glide;
    Shelf bump_l, bump_r;
    Top top_l, top_r;
    Top even_l, even_r;
    float dc_l0, dc_l1, dc_r0, dc_r1;
    float env_fast, env_slow;
    float gr, gr_k, atk, rel_fast, rel_slow;
    TetherBand band_a, band_b;
    Biquad ap_l, ap_r;
    float act, act_k, ap_hz;
    float t_att, t_rel;
    float makeup;
    float designed;
} Tape;

void tape_init(Tape *t, float sample_rate);
void tape_set(Tape *t, float warmth);
float tape_warmth(const Tape *t);
void tape_clear(Tape *t);
Stereo tape_process(Tape *t, Stereo x);

/* ---------- chandas ---------- */

#define CHANDAS_BUFFER_SECONDS 45.0f
#define CHANDAS_STREAMS 3
#define CHANDAS_MAX_GRAINS (CHANDAS_STREAMS * 2)
#define CHANDAS_MIN_BPM 20.0f
#define CHANDAS_MAX_BPM 300.0f
#define CHANDAS_DEFAULT_BPM 120.0f
#define CHANDAS_MIN_SIZE 0.1f
#define CHANDAS_MAX_SIZE 2.0f
#define CHANDAS_DIVISIONS_LEN 25
#define CHANDAS_DEFAULT_DIVISION 12
#define CHANDAS_TAP_SPREAD 0.72f
#define CHANDAS_RESET_FADE_SECONDS 0.012f

typedef struct {
    const char *name;
    float beats;
} Division;
extern const Division CHANDAS_DIVISIONS[CHANDAS_DIVISIONS_LEN];

typedef struct {
    bool enabled;
    float mix;
    bool sync;
    size_t division;
    float rate_hz;
    float spread;
    float size;
    float warp;
    float dimension;
    float tail;
} ChandasParams;
ChandasParams chandas_params_default(void);

float chandas_base_seconds_of(const ChandasParams *p, float bpm);
bool chandas_is_capped(const ChandasParams *p, float bpm);
float chandas_max_life(float delay, float rate, bool reverse, float buffer);
float chandas_dice(uint32_t n);

typedef struct {
    float *buf_l, *buf_r;
    size_t len;
    size_t w;
    float phase_l, phase_r;
    float depth;
    float sample_rate;
} Chorus;

typedef struct {
    float pos, step, age, life;
    float gain_l, gain_r;
    bool active;
} Grain;

typedef struct {
    ChandasParams params;
    bool transport_running;
    float sample_rate;
    float bpm;
    float *buf_l, *buf_r;
    size_t len;
    size_t w;
    Grain grains[CHANDAS_MAX_GRAINS];
    int slot[CHANDAS_STREAMS];
    float countdown[CHANDAS_STREAMS];
    float tap_pan[CHANDAS_STREAMS];
    Chorus chorus;
    Chamber chamber;
    float mix_s;
    float glide;
    DcBlock dc_loop_l, dc_loop_r, dc_out_l, dc_out_r;
    uint32_t spawned;
    float fade, fade_len;
    float harmony_interval;
    float since_pulse;
} Chandas;

void chandas_init(Chandas *h, float sample_rate);
void chandas_free(Chandas *h);
ChandasParams chandas_params(const Chandas *h);
void chandas_set_params(Chandas *h, ChandasParams p);
void chandas_set_transport(Chandas *h, bool running);
void chandas_set_tempo(Chandas *h, float bpm);
void chandas_note_pulse(Chandas *h);
float chandas_tempo(const Chandas *h);
void chandas_reset(Chandas *h);
float chandas_base_seconds(const Chandas *h);
Stereo chandas_process(Chandas *h, Stereo dry);

/* ---------- modulation: sequences ---------- */

#define SEQS 8
#define SEQ_STEPS 16
#define MOD_ROUTES 32
#define MOD_BLOCK 32 /* samples between control-rate updates */
#define SEQ_LENGTH_MIN_S 0.05f /* all 16 steps, when free */
#define SEQ_LENGTH_MAX_S 120.0f
#define SEQ_DEFAULT_DIVISION 21 /* 1/16: sixteen steps make a bar */
#define MOD_PITCH_SEMITONES 12.0f /* depth 1 on pitch */

typedef enum { SEQ_LOOP = 0, SEQ_ONCE, SEQ_MODE_COUNT } SeqMode;

/* Sixteen values played in time, moving whatever they are routed to. */
typedef struct {
    bool used;
    uint8_t mode;    /* SeqMode; ONCE restarts on every note */
    bool smooth;     /* a curve through the values, or each value held */
    int8_t division; /* one step's length: CHANDAS_DIVISIONS index, or -1 */
    float length_s;  /* all sixteen steps, when division is -1 */
    float value[SEQ_STEPS]; /* 0..1; 0.5 leaves the target where it is */
} SeqParams;

typedef enum {
    MT_NONE = 0,
    MT_INDEX, MT_RIP, MT_FB, MT_LEVEL, MT_PITCH,
    MT_MIX, MT_GHOST, MT_DECAY, MT_DAMP, MT_HAUNT,
    MT_CH_MIX, MT_CH_RATE, MT_CH_SPREAD, MT_CH_SIZE, MT_CH_WARP, MT_CH_DIM,
    MT_CH_TAIL, MT_MEL_RATE,
    MT_COUNT
} ModTarget;

typedef struct {
    const char *name;
    float min, max; /* depth 1 spans the whole range; pitch is in semitones */
} ModTargetSpec;
extern const ModTargetSpec MOD_TARGETS[MT_COUNT];
const char *seq_mode_name(SeqMode m);

typedef struct {
    uint8_t seq;    /* 0-based */
    uint8_t target; /* ModTarget; MT_NONE = empty */
    float depth;    /* -1..1 */
    bool snap;      /* pitch only: round to whole semitones */
} ModRoute;

typedef struct {
    SeqParams seq[SEQS];
    ModRoute route[MOD_ROUTES];
} ModBank;

ModBank mod_bank_default(void); /* empty */
SeqParams seq_params_default(void); /* used, flat, looping a bar of 1/16 */
ModBank mod_bank_sanitize(ModBank b);
/* the route index for seq -> target, or -1 */
int mod_bank_find_route(const ModBank *b, int seq, ModTarget t);
bool seq_has_pitch_route(const ModBank *b, int seq);
/* seconds one step lasts */
float seq_step_seconds(const SeqParams *p, float bpm);
/* the value (0..1) at pos steps in, 0..16. Held gives each step's value;
   smooth is a monotone curve through the step centres, so it never passes a
   value on either side of it. LOOP wraps round, ONCE holds its ends. */
float seq_value_at(const SeqParams *p, float pos);

typedef enum {
    SEQ_FILL_FLAT, SEQ_FILL_SINE, SEQ_FILL_SAW, SEQ_FILL_RAMP, SEQ_FILL_TRI,
    SEQ_FILL_SQUARE, SEQ_FILL_RANDOM, SEQ_FILL_COUNT
} SeqFill;
const char *seq_fill_name(SeqFill f);
/* paints the values; random uses seed so the same seed paints the same */
void seq_fill(SeqParams *p, SeqFill f, uint32_t seed);

typedef struct {
    float pos;   /* steps, 0..16 */
    float value; /* the output, -1..1 */
    bool done;   /* ONCE reached its end */
} SeqState;

/* the values a modulated engine reads, before and after modulation */
typedef struct {
    Patch patch;
    VerbParams verb;
    ChandasParams chandas;
    MelodyParams melody;
    float warmth;
    float bend; /* semitones */
} ModBase;

enum { MOD_G_PATCH = 1, MOD_G_VERB = 2, MOD_G_CHANDAS = 4, MOD_G_MELODY = 8,
       MOD_G_WARMTH = 16, MOD_G_BEND = 32 };

typedef struct {
    ModBank bank;
    SeqState st[SEQS];
    float sample_rate;
    int groups;      /* MOD_G_* touched by live routes */
    int groups_prev; /* touched on the previous tick */
} Mod;

void mod_init(Mod *m, float sample_rate);
void mod_set_seq(Mod *m, int slot, SeqParams p);
void mod_set_route(Mod *m, int slot, ModRoute r);
/* restarts every ONCE sequence */
void mod_note_on(Mod *m);
bool mod_any_seq(const Mod *m);
/* advances every sequence by samples */
void mod_advance(Mod *m, size_t samples, float bpm);
/* base plus every route; returns the groups that need writing to the engine,
   including groups a route just left so they go back to base */
int mod_apply(Mod *m, const ModBase *base, ModBase *out);

/* plays a pitch sequencer event on the voices, the way the melody's notes
   are played: one note at a time, the last one let go first. A gate-off
   releases only the sequencer's note (key -1). */
void pitch_event_play(PitchEvent e, VoiceBank *v, Chandas *h, Mod *m);

/* ---------- session ---------- */

#define START_HZ 110.0f

typedef struct {
    Patch patch;
    VerbParams verb;
    MelodyParams melody;
    float drone_hz;
    ChandasParams chandas;
    float tempo_bpm;
    float warmth;
    bool limiter_enabled;
    float limiter_ceiling_db;
    /* the note envelope (State.adsr), which lives outside Patch */
    float attack_s, decay_s, sustain, release_s;
    bool drone;
    ModBank mods;
    PitchSeqParams pitch;
} Session;

Session session_default(void);
Session session_sanitize(Session s);

/* ---------- inline helpers ---------- */

#include <math.h>
/* The one path a changing value takes. Nothing in the engine arrives
   instantly: a value written straight into a live signal is a click, and at
   modern speeds a glide short enough to feel immediate costs nothing. New
   parameters go through here rather than rolling their own. */
/* A tail fading to silence passes through subnormal floats, which are slow
   and which hosts reject as invalid output; anything this quiet is silence. */
static inline float flush_tiny(float x) {
    return fabsf(x) < 1e-20f ? 0.0f : x;
}

static inline float glide_k(float seconds, float sample_rate) {
    return 1.0f - expf(-1.0f / fmaxf(seconds * fmaxf(sample_rate, 1.0f), 1.0f));
}
static inline float glide_to(float now, float target, float k) {
    return now + (target - now) * k;
}
static inline float fract_pos(float x) { return x - floorf(x); }

/* Sine of a phase in cycles, for a slow modulation. A truncated Taylor on a
   folded quadrant: the error against libm sits under a millionth. On a delay
   wobble of a few samples, that is nowhere.
   Not an oscillator. */
static inline float lfo_sin(float phase) {
    float p = phase;
    float sign = 1.0f;
    if (p >= 0.5f) {
        p -= 0.5f;
        sign = -1.0f;
    }
    if (p > 0.25f) p = 0.5f - p;
    float x = p * TAU_F;
    float z = x * x;
    float s = -1.0f / 39916800.0f;
    s = s * z + 1.0f / 362880.0f;
    s = s * z - 1.0f / 5040.0f;
    s = s * z + 1.0f / 120.0f;
    s = s * z - 1.0f / 6.0f;
    s = s * z + 1.0f;
    return sign * s * x;
}
/* NaN fails both comparisons, so it has to be rejected explicitly */
static inline float clampf(float x, float lo, float hi) {
    return isnan(x) ? lo : (x < lo ? lo : (x > hi ? hi : x));
}

#endif /* BYPO_DSP_H */
