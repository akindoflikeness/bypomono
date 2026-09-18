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

/* ---------- gate ---------- */

#define GATE_GLIDE_S 0.013f
#define GATE_FLOOR 1e-6f

typedef struct { float gain, k; } EngageGate;
void engage_gate_init(EngageGate *g, float sample_rate, bool engaged);
float engage_gate_next(EngageGate *g, bool open);

/* ---------- algorithm ---------- */

#define NUM_OPS 5
#define NUM_NODES 4
#define ALGORITHM_SPACE 81

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
    float field;
    float curve;
    uint8_t voices;      /* notes at once: 1 = mono, up to POLY_MAX */
    uint8_t unison;      /* voices per note: 1 or UNISON_MAX */
    float unison_detune; /* cents between the two unison voices */
} Patch;

Patch patch_init(AlgorithmId algorithm, RatioMode ratio_mode);

/* ---------- envelope ---------- */

typedef struct {
    float attack_s, decay_s, release_s, curve;
    float sustain; /* fraction of the velocity-derived sustain level */
} EnvParams;
EnvParams env_params_default(void); /* 0.008, 2.0, 2.0, 0.5, 1.0 */

#define ENV_TIME_MAX 8.0f
#define ENV_RELEASE_MIN 0.05f

#define ENV_FLOOR 1e-4f
#define VELOCITY_CEILING 0.8f
float velocity_for_level(float position);

typedef enum { ENV_IDLE, ENV_HELD, ENV_RELEASED } EnvStage;

typedef struct {
    EnvStage stage;
    float t, from, level, peak, sustain, sample_rate;
} Envelope;

void envelope_init(Envelope *e, float sample_rate);
void envelope_note_on(Envelope *e, float velocity);
void envelope_note_off(Envelope *e);
bool envelope_active(const Envelope *e);
float envelope_level(const Envelope *e);
float envelope_amplitude(const Envelope *e, float trim);
float envelope_tick(Envelope *e, const EnvParams *p);

/* ---------- breath ---------- */

#define BREATH_RATE_RUNGS 13
#define BREATH_RATE_MIN_HZ (27.5f / 521.002f)
#define BREATH_RATE_MAX_HZ (440.0f / 521.002f)
#define BREATH_DECLICK_S 0.003f
#define BREATH_CURVE_RANGE 4.0f

float curve_exponent(float curve);

typedef struct {
    float sample_rate;
    float phase[5];
    RatioMode mode;
    bool has_trigger;      /* Option<f32> since_trigger */
    float since_trigger;
    float boost_level;
    float interval;
    float last_amount;
    float declick_from;
    float declick_left;
} Breath;

typedef struct { float amount, gain, pitch; } Field;

void breath_init(Breath *b, float sample_rate, RatioMode mode);
void breath_set_mode(Breath *b, RatioMode mode);
void breath_trigger(Breath *b);
void breath_release(Breath *b);
float breath_rate_hz(float freq);
Field breath_tick(Breath *b, float freq, float field, float floor_, float curve);
void breath_reset(Breath *b);

/* ---------- state ---------- */

typedef enum { AMP_DRONE = 0, AMP_ENVELOPE } AmpKind;

typedef struct {
    AmpKind kind;
    EnvParams env; /* meaningful when kind == AMP_ENVELOPE */
} AmpSource;

typedef struct { AmpSource amp; } Chain;
Chain chain_default(void); /* AMP_DRONE */

typedef struct {
    Patch patch;
    Chain chain;
} State;

State state_new(Patch patch);
bool chain_is_structural_change(const Chain *a, const Chain *b);
bool state_is_structural_change(const State *a, const State *b);

/* ---------- voice ---------- */

typedef struct {
    float ops[NUM_OPS];
    float mix;
    float master;
    float field;
    float base_hz;
    float side; /* dry mix difference: left gets mix + side, right mix - side */
} Frame;

typedef void (*FrameEmit)(void *userdata, size_t n, const Frame *frame);

typedef struct {
    float *buf;
    size_t len;
    size_t write;
    size_t delay;
    PhaseRotator rot;
    float rot_hz; /* pitch the rotator is currently solved for */
    float lp;
    float fb;
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
    float master, index;
    float fb_smooth; /* glided, so modulating fb cannot zipper */
    RipLine rip_line;
    float rip_sig;
    Breath breath;
    float field_smooth, curve_smooth, field_amount, field_pitch;
    float bend;
    float detune; /* frequency ratio from the unison spread */
    Chain chain;
    Envelope env;
    float master_pos;
} Voice;

void voice_init(Voice *v, float sample_rate, Patch patch); /* allocates rip buffer */
void voice_free(Voice *v);
void voice_note_off(Voice *v);
void voice_set_freq_hz(Voice *v, float hz);
void voice_set_drone_hz(Voice *v, float hz); /* retunes the rip rotator */
void voice_set_chain(Voice *v, Chain chain);
void voice_note_on(Voice *v, float hz, float velocity);
bool voice_note_sounding(const Voice *v);
void voice_set_bend_semitones(Voice *v, float semitones);
void voice_set_detune_cents(Voice *v, float cents);
/* clears the rip line's memory before a silent voice takes a new note */
void voice_wake(Voice *v);
float voice_target_hz(const Voice *v);
void voice_glide_to_hz(Voice *v, float hz);
void voice_set_op_enabled(Voice *v, int op, bool on);
void voice_set_patch(Voice *v, Patch patch);
void voice_set_algorithm(Voice *v, AlgorithmId algorithm);
float voice_op_phase(const Voice *v, int op);
void voice_render_frames(Voice *v, size_t count, FrameEmit emit, void *userdata);
void voice_render(Voice *v, float *buf, size_t len);

/* ---------- pair ---------- */

#define CROSSFADE_SECONDS 0.035f

typedef struct {
    Voice voices[2];
    int target;
    float blend, step;
    float sample_rate;
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
void voice_pair_note_on(VoicePair *p, float hz, float velocity);
bool voice_pair_note_sounding(const VoicePair *p);
const Chain *voice_pair_chain(const VoicePair *p);
void voice_pair_set_bend_semitones(VoicePair *p, float semitones);
void voice_pair_note_off(VoicePair *p);
float voice_pair_target_hz(const VoicePair *p);
void voice_pair_render_frames(VoicePair *p, size_t count, FrameEmit emit, void *userdata);
void voice_pair_set_chain_now(VoicePair *p, Chain chain); /* no crossfade */
void voice_pair_set_detune_cents(VoicePair *p, float cents);
void voice_pair_wake(VoicePair *p);
/* both voices are note-driven and their envelopes have finished */
bool voice_pair_silent(const VoicePair *p);

/* ---------- bank ---------- */

#define BANK_PAIRS (POLY_MAX * UNISON_MAX)
#define BANK_CHUNK 128
#define UNISON_WIDTH 0.5f

/* Up to POLY_MAX notes, each played by UNISON_MAX crossfading pairs.
   Slot = note * UNISON_MAX + copy. The drone always plays on note 0. */
typedef struct {
    VoicePair pairs[BANK_PAIRS];
    float gain[BANK_PAIRS];
    int key[POLY_MAX]; /* -1 = no key (sequencer or drone) */
    bool held[POLY_MAX];
    uint32_t stamp[POLY_MAX];
    uint32_t clock;
    int newest;
    int poly; /* note slots reachable under the current patch and chain */
    float spread, step;
    Frame scratch[BANK_PAIRS][BANK_CHUNK];
} VoiceBank;

void voice_bank_init(VoiceBank *b, float sample_rate, Patch patch);
void voice_bank_free(VoiceBank *b);
bool voice_bank_crossing(const VoiceBank *b);
State voice_bank_state(const VoiceBank *b);
void voice_bank_set_state(VoiceBank *b, State next);
void voice_bank_set_patch(VoiceBank *b, Patch patch);
void voice_bank_set_chain_now(VoiceBank *b, Chain chain);
const Patch *voice_bank_patch(const VoiceBank *b);
const Compiled *voice_bank_compiled(const VoiceBank *b);
const Chain *voice_bank_chain(const VoiceBank *b);
int voice_bank_poly(const VoiceBank *b);
void voice_bank_set_freq_hz(VoiceBank *b, float hz);
void voice_bank_set_drone_hz(VoiceBank *b, float hz);
void voice_bank_glide_to_hz(VoiceBank *b, float hz); /* the drone, note 0 */
void voice_bank_note_on(VoiceBank *b, int key, float hz, float velocity);
/* mono releases whatever sounds; poly releases the note on `key`, or every
   held note for key -1 */
void voice_bank_note_off(VoiceBank *b, int key);
void voice_bank_note_off_all(VoiceBank *b);
bool voice_bank_note_sounding(const VoiceBank *b);
void voice_bank_set_bend_semitones(VoiceBank *b, float semitones);
float voice_bank_target_hz(const VoiceBank *b); /* the newest note */
const Envelope *voice_bank_newest_env(const VoiceBank *b);
/* target hz of each held note (the drone counts as held); returns count */
int voice_bank_held_hz(const VoiceBank *b, float out[POLY_MAX]);
void voice_bank_render_frames(VoiceBank *b, size_t count, FrameEmit emit, void *userdata);

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
#define CHAMBER_DRIVE 2.08f
#define CHAMBER_DAMP 0.27f
#define CHAMBER_ASYM 0.75f
#define CHAMBER_WIDTH 1.47f
#define CHAMBER_MOD_CENTS 9.3f
#define CHAMBER_PREDELAY_MS 130.0f
#define CHAMBER_HPF_HZ 38.0f

void hadamard8(float s[CHAMBER_N]);
float chamber_jfet(float x, float asym);

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
} MelodyParams;
MelodyParams melody_params_default(void);

typedef struct {
    MelodyParams params;
    float sample_rate;
    size_t countdown;
    float weyl;
    uint32_t rng;
    float walk_hz;
} Melody;

void melody_init(Melody *m, float sample_rate, MelodyParams params);
void melody_set_params(Melody *m, MelodyParams params);
bool melody_samples_until_fire(const Melody *m, size_t *out);
void melody_advance(Melody *m, size_t samples);
float melody_fire(Melody *m);

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
    float delay_seconds;
    float fb, fb_target, lp;
    float lfo_phase, lfo_inc;
} Comb;

typedef struct {
    float *buf;
    size_t len;
    size_t write;
    size_t delay;
    PhaseRotator rot;
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
    float send[NUM_OPS];
    bool is_carrier[NUM_OPS];
    Comb combs_l[NUM_OPS], combs_r[NUM_OPS];
    Allpass ap_l[2], ap_r[2];
    GhostLine ghosts[NUM_OPS];
    DcBlock dc[NUM_OPS];
    DcBlock wet_dc_l, wet_dc_r;
    Svf room_hp_l, room_hp_r;
    float room_hp_g, room_hp_for_hz;
    float ghost_hz; /* pitch the ghost rotators are currently solved for */
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
void chandas_set_tempo(Chandas *h, float bpm);
void chandas_note_pulse(Chandas *h);
float chandas_tempo(const Chandas *h);
void chandas_reset(Chandas *h);
float chandas_base_seconds(const Chandas *h);
Stereo chandas_process(Chandas *h, Stereo dry);

/* ---------- modulation ---------- */

#define MOD_LFOS 16
#define MOD_ROUTES 32
#define MOD_BLOCK 32 /* samples between control-rate updates */
#define LFO_RATE_MIN_HZ 0.01f
#define LFO_RATE_MAX_HZ 40.0f
#define MOD_PITCH_SEMITONES 12.0f /* depth 1 on pitch */

typedef enum {
    LFO_SINE = 0, LFO_TRIANGLE, LFO_SAW, LFO_RAMP, LFO_SQUARE, LFO_EXP,
    LFO_SH, LFO_DRIFT, LFO_SHAPE_COUNT
} LfoShape;

typedef enum { LFO_FREE = 0, LFO_RETRIG, LFO_ONCE, LFO_MODE_COUNT } LfoMode;

typedef struct {
    bool used;
    uint8_t shape;    /* LfoShape */
    uint8_t mode;     /* LfoMode */
    bool unipolar;
    int8_t division;  /* index into CHANDAS_DIVISIONS, or -1 for rate_hz */
    float rate_hz;
    float phase;      /* start phase, 0..1 */
} LfoParams;

typedef enum {
    MT_NONE = 0,
    MT_INDEX, MT_RIP, MT_FB, MT_FIELD, MT_CURVE, MT_LEVEL, MT_PITCH,
    MT_MIX, MT_GHOST, MT_DECAY, MT_DAMP, MT_HAUNT,
    MT_CH_MIX, MT_CH_RATE, MT_CH_SPREAD, MT_CH_SIZE, MT_CH_WARP, MT_CH_DIM,
    MT_CH_TAIL, MT_MEL_RATE, MT_WARMTH,
    MT_COUNT
} ModTarget;

typedef struct {
    const char *name;
    float min, max; /* depth 1 spans the whole range; pitch is in semitones */
} ModTargetSpec;
extern const ModTargetSpec MOD_TARGETS[MT_COUNT];
const char *lfo_shape_name(LfoShape s);
const char *lfo_mode_name(LfoMode m);

typedef struct {
    uint8_t lfo;    /* 0-based slot */
    uint8_t target; /* ModTarget; MT_NONE = empty */
    float depth;    /* -1..1 */
} ModRoute;

typedef struct {
    LfoParams lfo[MOD_LFOS];
    ModRoute route[MOD_ROUTES];
} ModBank;

ModBank mod_bank_default(void); /* empty */
LfoParams lfo_params_default(void);
ModBank mod_bank_sanitize(ModBank b);
/* the route index for lfo -> target, or -1 */
int mod_bank_find_route(const ModBank *b, int lfo, ModTarget t);
/* rate in hz after tempo sync */
float lfo_effective_hz(const LfoParams *p, float bpm);
/* the waveform at a phase, -1..1 bipolar or 0..1 unipolar; random shapes
   use seed so a drawing of them is stable */
float lfo_shape_at(const LfoParams *p, float phase, uint32_t seed);

typedef struct {
    float phase;
    float value;
    float from, to; /* random shapes: the held value and the next one */
    uint32_t rng;
    bool done;      /* LFO_ONCE finished its cycle */
} LfoState;

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
    LfoState st[MOD_LFOS];
    float sample_rate;
    int groups;      /* MOD_G_* touched by live routes */
    int groups_prev; /* touched on the previous tick */
} Mod;

void mod_init(Mod *m, float sample_rate);
void mod_set_lfo(Mod *m, int slot, LfoParams p);
void mod_set_route(Mod *m, int slot, ModRoute r);
void mod_note_on(Mod *m);
bool mod_any_lfo(const Mod *m);
/* advances every lfo by samples */
void mod_advance(Mod *m, size_t samples, float bpm);
/* base plus every route; returns the groups that need writing to the engine,
   including groups a route just left so they go back to base */
int mod_apply(Mod *m, const ModBase *base, ModBase *out);

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
    /* the note envelope (Chain.amp.env), which lives outside Patch; its
       curve is Patch.curve */
    float attack_s, decay_s, sustain, release_s;
    bool drone;
    ModBank mods;
} Session;

Session session_default(void);
Session session_sanitize(Session s);

/* ---------- inline helpers ---------- */

#include <math.h>
static inline float fract_pos(float x) { return x - floorf(x); }
/* NaN fails both comparisons, so it has to be rejected explicitly */
static inline float clampf(float x, float lo, float hi) {
    return isnan(x) ? lo : (x < lo ? lo : (x > hi ? hi : x));
}

#endif /* BYPO_DSP_H */
