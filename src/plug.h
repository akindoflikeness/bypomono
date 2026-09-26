#ifndef BYPO_PLUG_H
#define BYPO_PLUG_H

#include <clap/clap.h>
#include <stdatomic.h>

#include "gui/app.h"

enum {
    P_DRONE, P_DRONE_HZ, P_ALGORITHM, P_RATIO_MODE, P_INDEX, P_RIP, P_FB,
    P_GLIDE, P_FIELD, P_CURVE, P_RELEASE, P_LEVEL,
    P_OP1, P_OP2, P_OP3, P_OP4, P_OP5,
    P_MIX, P_GHOST, P_DECAY, P_DAMP, P_HAUNT,
    P_CH_ON, P_CH_MIX, P_CH_SYNC, P_CH_DIV, P_CH_RATE, P_CH_SPREAD, P_CH_SIZE,
    P_CH_WARP, P_CH_DIM, P_CH_TAIL,
    P_SH_ON, P_SH_SRC, P_SH_TUNING, P_SH_SCALE, P_SH_ROOT, P_SH_RANGE,
    P_SH_RATE,
    P_WARMTH,
    P_POLY, P_UNISON, P_DETUNE,
    P_ATTACK, P_ENV_DECAY, P_SUSTAIN,
    /* Append-only: existing CLAP parameter IDs are part of saved host state. */
    P_OP_RATIO1, P_OP_RATIO2, P_OP_RATIO3, P_OP_RATIO4, P_OP_RATIO5,
    P_OP_LEVEL1, P_OP_LEVEL2, P_OP_LEVEL3, P_OP_LEVEL4, P_OP_LEVEL5,
    P_LIMITER, P_LIMITER_CEILING,
    P_OUTPUT_GAIN,
    P_COUNT
};

typedef enum { K_FLOAT, K_STEP, K_ONOFF } ParamKind;

typedef struct {
    const char *name;
    const char *module;
    double min, max, def;
    ParamKind kind;
} ParamSpec;

extern const ParamSpec PLUG_SPEC[P_COUNT];

typedef struct Plug {
    clap_plugin_t plugin;
    const clap_host_t *host;
    double sr;
    bool active;
    _Atomic double vals[P_COUNT];
    _Atomic bool dirty;
    /* audio-thread engine */
    VoiceBank voice;
    StereoVerb verb;
    Melody melody;
    PitchSeq pitch;
    Chandas chandas;
    Tape tape;
    Limiter limiter;
    Mod mod;
    ModBase base; /* what the params say, before modulation */
    bool engine_alive;
    bool transport_running;
    float applied_drone_hz;
    /* main thread writes the sequences and routes host state saves. bank_seq
       is a seqlock (odd while a write is in progress). bank_gen advances when
       a state load should replace whatever the audio thread is playing. */
    ModBank mods_main;
    PitchSeqParams pitch_main;
    _Atomic uint32_t bank_seq;
    _Atomic uint32_t bank_gen;
    uint32_t bank_seen; /* audio thread: last bank_gen it applied */
    EventRing mod_ev;
    /* melody clock, not a host param: low 8 bits are the division as int8,
       bit 8 is sync. Written by both threads, so it is one atomic. */
    _Atomic uint32_t mel_clock;
    /* editor bridge */
    _Atomic(App *) gui_app;
    bool rec_on;
    uint32_t viz_decim;
    float peak_acc[2];
    _Atomic bool host_touched; /* host moved params; editor shadows stale */
    void *gui_state;           /* owned by plug_gui.c */
    bool host_held[128]; /* keys the host holds; gates must not cut them */
} Plug;

double plug_getv(const Plug *p, int id);
void plug_setv(Plug *p, int id, double v);
Session plug_session_of_vals(const Plug *p);
/* main thread: publish a modulation bank and pitch sequence. Does not ask
   the audio thread to reload; state_load does that itself. */
void plug_publish_bank(Plug *p, const ModBank *mods, const PitchSeqParams *pitch);

/* X11 alone delivers editor input on a pollable fd */
#if defined(_WIN32) || defined(__APPLE__)
#define BYPO_GUI_POSIX_FD 0
#else
#define BYPO_GUI_POSIX_FD 1
#endif

/* plug_gui.c */
extern const clap_plugin_gui_t PLUG_EXT_GUI;
extern const clap_plugin_timer_support_t PLUG_EXT_TIMER;
#if BYPO_GUI_POSIX_FD
extern const clap_plugin_posix_fd_support_t PLUG_EXT_FD;
#endif

#endif
