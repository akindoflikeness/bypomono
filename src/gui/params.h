#ifndef BYPO_PARAMS_H
#define BYPO_PARAMS_H

#include <stdbool.h>
#include <stddef.h>

struct App;

/* How a fader's travel (0..1) maps onto the value. */
typedef enum {
    CURVE_LINEAR,
    CURVE_LOG,      /* equal ratios along the travel */
    CURVE_POWER,    /* lo + (hi - lo) * pos^shape */
    CURVE_ENV_TIME, /* lo at the bottom, then equal ratios up to hi */
} Curve;

/* what has to be sent to the engine after a value changes */
typedef enum {
    PG_PATCH = 1, PG_VERB = 2, PG_MELODY = 4, PG_CHANDAS = 8,
    PG_WARMTH = 16, PG_LIMITER = 32, PG_DRONE = 64, PG_ENV = 128
} ParamGroup;

typedef enum {
    PARAM_INDEX, PARAM_RIP, PARAM_FB, PARAM_GLIDE, PARAM_FIELD, PARAM_CURVE,
    PARAM_LEVEL, PARAM_DETUNE, PARAM_DRONE_HZ,
    PARAM_MIX, PARAM_GHOST, PARAM_VERB_DECAY, PARAM_DAMP, PARAM_HAUNT,
    PARAM_ATTACK, PARAM_ENV_DECAY, PARAM_SUSTAIN, PARAM_RELEASE,
    PARAM_WARMTH, PARAM_CEILING,
    PARAM_MEL_RATE, PARAM_MEL_RANGE, PARAM_MEL_ROOT,
    PARAM_CH_RATE, PARAM_CH_MIX, PARAM_CH_SPREAD, PARAM_CH_SIZE,
    PARAM_CH_WARP, PARAM_CH_DIM, PARAM_CH_TAIL,
    PARAM_COUNT
} ParamId;

/* One row per continuous setting. Faders, the console and MIDI CC all take
   their range, curve and default from here. */
typedef struct {
    const char *name;  /* console word; grouped ones carry their group */
    const char *label; /* fader text */
    float lo, hi;
    Curve curve;
    float shape; /* CURVE_POWER exponent */
    bool integer;
    const char *fmt; /* fader value text; NULL = seconds or ms */
    const char *unit, *alt_unit; /* console suffixes */
    int group; /* ParamGroup bits */
} Control;

extern const Control PARAMS[PARAM_COUNT];

int param_find(const char *name); /* -1 when there is none */
float param_get(const struct App *a, ParamId id);
/* clamps to the range and rounds integers */
void param_set(struct App *a, ParamId id, float v);
float param_default(const struct App *a, ParamId id);
float param_pos(ParamId id, float v); /* value -> travel */
float param_at(ParamId id, float pos); /* travel -> value */
void param_text(const struct App *a, ParamId id, char *out, size_t cap);
/* sends every group in the mask to the engine */
void params_send(struct App *a, int groups);

float log_position(float p, float lo, float hi);
float position_of_log(float v, float lo, float hi);
/* envelope times: `lo` at the bottom of the travel, then equal ratios from
   ENV_TIME_FLOOR (or lo, if higher) up to ENV_TIME_MAX */
#define ENV_TIME_FLOOR 0.001f
float env_time_at(float t, float lo);
float env_time_pos(float v, float lo);

#endif
