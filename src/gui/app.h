#ifndef BYPO_APP_H
#define BYPO_APP_H

#include <stdatomic.h>
#include <stdio.h>

#include "../audio.h"
#include "../cli/view.h"
#include "../dsp/dsp.h"
#include "../midi.h"
#include "../ring.h"
#include "canvas.h"
#include "text.h"
#include "ui.h"

/* ---------- design grid ---------- */

#define DESIGN_W 1180.0f
#define DESIGN_H 780.0f
#define MIN_WINDOW_W 233.0f
#define MIN_WINDOW_H 144.0f

/* Quarter steps: the blit is nearest-neighbour, so a fractional factor
   gives alternating fat/thin pixel rows rather than blur. */
#define WINDOW_SCALE_MIN 1.0f
#define WINDOW_SCALE_MAX 4.0f
#define WINDOW_SCALE_STEP 0.25f

#define TIGHT 2.0f
#define SNUG 3.0f
#define GAP 5.0f
#define GROUP 8.0f
#define SECTION 13.0f
#define FADER_H 23.0f
#define FOOTER_LINE_H 23.0f
#define PRESET_STRIP_H 23.0f /* top of column three; fits the 21px preset bar */
#define ROOM_ART_H 144.0f
#define TREE_H 150.0f
#define ENGAGE_H 34.0f
#define CELL_GUTTER GAP
#define HINT_CHIP_H 15.0f
#define HINT_ROW_H 21.0f
#define FOOTER_OPEN_H 144.0f
#define SEAM_GRAB 9.0f
#define VIZ_DECIMATE 4
#define REPAINT_FLOOR_MS 10
#define GOLDEN_MAJOR (1.0f / PHI)
#define OPS_COL_W 250.0f
#define STATS_COL_W 233.0f
#define CONTROLS_HOUSE_W 325.0f
#define REFERENCE_ROW_W DESIGN_W
#define REFERENCE_CENTRE_W (REFERENCE_ROW_W - STATS_COL_W - OPS_COL_W)
#define OPS_COL_SHARE (OPS_COL_W / REFERENCE_ROW_W)
#define STATS_COL_SHARE (STATS_COL_W / REFERENCE_ROW_W)
#define CONTROLS_SHARE (CONTROLS_HOUSE_W / REFERENCE_CENTRE_W)
#define PRESET_NAME_W 178.0f /* design-grid pixels; room for preset names/search */
#define INFO_BUTTON_SIDE 9.0f /* source pixels of the built-in 9x9 cog */
#define INFO_BUTTON_W (INFO_BUTTON_SIDE + 2.0f * GAP)
#define BEND_SEMITONES 2.0f
#define VEIL 0.5f
#define APP_VERSION "1.1.2"

/* scale.c */
/* largest quarter step whose magnified grid fits avail_w x avail_h less
   window chrome; BYPO_SCALE overrides */
float pick_display_scale(int avail_w, int avail_h);
/* nearest quarter step, clamped to the same range */
float snap_scale(float s);

/* ---------- audio<->ui protocol ---------- */

typedef enum {
    EV_SET_PATCH, EV_SET_VERB, EV_SET_MELODY, EV_SET_CHANDAS, EV_SET_WARMTH,
    EV_RESET_CHANDAS, EV_SET_TEMPO, EV_GLIDE_TO, EV_RECORD, EV_NOTE_OFF,
    EV_ENGAGE, EV_BEND, EV_NOTE_ON, EV_SET_CHAIN, EV_SET_MIDI_DRIVING,
    EV_SET_LFO, EV_SET_ROUTE, EV_SET_TRANSPORT, EV_PANIC
} EventKind;

typedef struct {
    EventKind kind;
    union {
        Patch patch;
        VerbParams verb;
        MelodyParams melody;
        ChandasParams chandas;
        Chain chain;
        float f;
        bool flag;
        struct { float hz, velocity; int key; } note; /* key -1 = none */
        struct { int slot; LfoParams p; } lfo;
        struct { int slot; ModRoute r; } route;
    } u;
} Event;

/* what the audio thread's lfos are doing, for drawing. the random shapes
   have no picture to walk, so their own output is kept and drawn as a scope */
#define LFO_HIST 256
#define LFO_HIST_EVERY 8 /* control blocks between samples */

typedef struct {
    _Atomic uint32_t phase_q16[MOD_LFOS];
    _Atomic uint32_t value_bits[MOD_LFOS]; /* float bits */
    _Atomic int32_t hist[MOD_LFOS][LFO_HIST]; /* value * 127 */
    _Atomic uint32_t hist_head[MOD_LFOS];
    uint32_t skip;
} LfoMeter;
void lfo_meter_store(LfoMeter *m, const Mod *mod);
float lfo_meter_phase(const LfoMeter *m, int slot);
float lfo_meter_value(const LfoMeter *m, int slot);
/* the last n samples, oldest first; returns how many were written */
int lfo_meter_history(const LfoMeter *m, int slot, float *out, int n);

typedef struct {
    float ops[NUM_OPS];
    float l, r;
    float peak[2];
} VizFrame;

RING_DECLARE(EventRing, Event, 256)
RING_DECLARE(VizRing, VizFrame, 16384)
RING_DECLARE(RecRing, float, (1 << 19))

/* fixed-point atomics, all relaxed */
typedef struct { _Atomic int32_t note; } MidiNoteAtom; /* -1 = none */
void midi_note_press(MidiNoteAtom *m, uint8_t note);
void midi_note_release(MidiNoteAtom *m, uint8_t note);
void midi_note_clear(MidiNoteAtom *m);
int midi_note_get(const MidiNoteAtom *m);

typedef struct { _Atomic uint32_t q16; } PitchAtom; /* hz * 65536 */
void pitch_store(PitchAtom *p, float hz);
float pitch_load(const PitchAtom *p);

typedef struct { _Atomic uint32_t word[128]; } CcState; /* (seq<<8)|value */
void cc_write(CcState *c, uint8_t cc, uint8_t value);
uint32_t cc_read(const CcState *c, uint8_t cc);
float cc_position(uint32_t word);

typedef struct {
    _Atomic uint32_t load_q16, peak_q16, frames;
} AudioMeter;

/* ---------- recorder ---------- */

typedef struct {
    FILE *file;
    char path[512];
    char preset_slug[128];
    uint64_t frames;
    float sample_rate;
} Recorder;

bool recorder_start(Recorder *r, const char *preset_name, float sample_rate);
void recorder_push(Recorder *r, const float *interleaved, size_t samples);
void recorder_finalize(Recorder *r);

/* ---------- presets ---------- */

#define MAX_PRESETS 512
#define MAX_FOLDERS 32
#define MINE_BANK "USER"
#define STOCK_BANK "BYPO"
#define DEFAULT_PRESET "init"
#define TRASH_DIR "trash" /* deleted presets go here; the browser never lists it */

typedef struct {
    char bank[64]; /* "" = loose (USER view) */
    char name[128];
} PresetRef;

typedef enum { FILTER_ALL, FILTER_MINE, FILTER_BANK } PresetFilterKind;
typedef struct {
    PresetFilterKind kind;
    char bank[64];
} PresetFilter;

/* keyboard walk over the compact previous/next preset buttons */
#define PRESET_BUTTONS 2

/* json_session.c */
/* a preset or state document larger than this is refused unread */
#define SESSION_JSON_MAX (1u << 20)
bool session_from_json(const char *json, Session *out); /* sanitized */
/* serde_json-compatible pretty output; caller frees */
char *session_to_json(const Session *s);

/* presets.c */
const char *preset_dir(void);
const char *recording_dir(void);
const char *user_data_root(void);
const char *asset_dir(void); /* first existing of the asset search path */
void prepare_preset_dir(void);
void sanitise_segment(const char *raw, char *out, size_t out_len); /* "" = refused */
bool preset_path(const PresetRef *r, char *out, size_t out_len);
const char *preset_bank_label(const PresetRef *r);
void preset_qualified(const PresetRef *r, char *out, size_t out_len);
/* reads and parses one session document; a file over SESSION_JSON_MAX is
   refused without being read */
bool session_load_file(const char *path, Session *out);

/* ---------- the app ---------- */

typedef enum { DREAD_NORMAL, DREAD_LOW, DREAD_CRITICAL } Dread;

typedef enum {
    CC_NONE = 0, CC_INDEX, CC_RIP, CC_FB, CC_FIELD, CC_CURVE, CC_RELEASE,
    CC_GLIDE, CC_DRONEHZ, CC_MIX, CC_GHOST, CC_DECAY, CC_DAMP, CC_HAUNT,
    CC_WARMTH, CC_ATTACK, CC_ENVDECAY, CC_SUSTAIN
} CcTarget;
#define CC_LAST CC_SUSTAIN
const char *cc_target_name(CcTarget t);
CcTarget cc_target_from_name(const char *s);

typedef struct {
    float tree, ops, controls; /* <0 = house default */
    float top;                 /* <0 = house default */
} Splits;

#define LOG_LINES 64
#define LOG_LINE_LEN 256
#define PIN_MAX 6

typedef struct App {
    /* engine shadow state */
    Patch shadow;
    VerbParams shadow_verb;
    MelodyParams shadow_melody;
    ChandasParams shadow_chandas;
    float shadow_warmth;
    float shadow_attack_s, shadow_decay_s, shadow_sustain, shadow_release_s;
    float tempo_bpm;
    enum { TEMPO_INTERNAL, TEMPO_HOST, TEMPO_MIDI, TEMPO_PULSE, TEMPO_LINK }
        tempo_source;
    bool transport_running;
    float drone_hz;
    ModBank mods;
    Chain chain;
    bool engaged;
    bool restored;
    bool hosted; /* running as a plugin editor: host notes always may drive */

    /* audio rig */
    AudioOut audio;
    MidiIn midi;
    bool midi_open;
    char midi_port[128];
    EventRing ctrl, midi_ev;
    VizRing viz;
    RecRing rec;
    MidiNoteAtom midi_note;
    PitchAtom pitch;
    _Atomic uint32_t held_pcs; /* pitch classes of the held notes, bit per class */
    /* the newest note's envelope: stage << 30 | milliseconds into it, and
       its level in q16 */
    _Atomic uint32_t env_clock, env_level_q16;
    CcState cc;
    AudioMeter meter;
    LfoMeter lfo_meter;
    _Atomic bool rec_on;
    float sample_rate;
    int channels;

    /* timing / animation */
    double start_time;
    double last_frame_time;
    uint64_t frame_count;
    float fps;
    bool splash_over;
    float ripple_phase, suture_phase, rock_phase[3], spin, shell_scale;
    float index_smooth, dread_level, agitation;
    Dread dread;

    /* viz buffers */
    float env[NUM_OPS];
    float lissa_x[512], lissa_y[512];
    int lissa_len, lissa_head;

    /* cc mapping */
    CcTarget cc_bind[128];
    bool cc_heard[128];
    uint32_t cc_seen[128];

    /* layout */
    Splits splits;
    Rct preset_bar_rect;
    bool have_preset_bar_rect;

    /* preset bank */
    UiText preset_name;
    PresetRef preset_names[MAX_PRESETS];
    int preset_count;
    char preset_folders[MAX_FOLDERS][64];
    int folder_count;
    PresetFilter preset_filter;
    PresetRef preset_loaded, preset_selected;
    bool have_loaded, have_selected;
    bool preset_searching, preset_focus, presets_open, presets_were_open;
    int preset_button_at; /* bar button the keyboard walk is on */
    double preset_click_at;
    bool have_click_at;
    UiScroll preset_scroll;
    Rct presets_pane_rect; /* where the pane was drawn last frame */
    bool have_presets_pane_rect;

    /* console / log */
    char log[LOG_LINES][LOG_LINE_LEN];
    GraphPlace log_place[LOG_LINES]; /* a snapshot strip beside the line */
    Graph log_graph[LOG_LINES];
    int log_len, log_head; /* newest at head-1 */
    /* lines run with -v: they stay above the log and redraw every frame */
    char pins[PIN_MAX][LOG_LINE_LEN];
    bool pin_folded[PIN_MAX]; /* text only, no strips */
    int pin_count;
    bool console_open, console_focus, console_focused;
    UiText console_input;
    char console_typing[LOG_LINE_LEN];
    int console_revealed;
    float console_credit;
    int line_lit;                /* highlighted completion + 1; 0 = none */
    char line_seen[256];         /* the line line_lit was chosen against */
    uint32_t tips_told; /* bitmask by TipWhen */
    UiScroll log_scroll;

    /* panes */
    bool info_open, show_fps;
    int ops_tab, display_tab;
    UiScroll left_scroll, right_scroll;

    /* recording */
    Recorder recorder;
    bool recording;
    bool rec_stop_pending;
    uint32_t rec_quiet_frames;
    double rec_stop_at;
    bool have_rec_stop_at;

    P2 pointer;
    bool quit;
} App;

/* push onto the UI->audio ring, logging on overflow */
void app_send(App *a, Event ev);
/* every lfo and route slot, after a preset or state replaced a->mods */
void app_send_mods(App *a);
void push_log(App *a, const char *fmt, ...);
/* every line of a view, strips kept as snapshots */
void push_log_view(App *a, const View *v);

/* gui_engine.c */
int gui_audio_start(App *a);
void gui_audio_stop(App *a);
void gui_drain_viz(App *a);
void gui_drain_recording(App *a);
void gui_apply_cc(App *a, Ui *ui);
void gui_sync_chain(App *a);
bool midi_driving(const App *a);
int midi_port_names(char names[][128], int max);
bool gui_set_midi_port(App *a, const char *name); /* NULL = close */
/* audio thread: newest pitch and held pitch classes for the keyboard */
void voices_store(App *a, const VoiceBank *b);
void gui_run_record(App *a, const char *args);    /* console verb */
void gui_stop_record(App *a);
void gui_run_bind(App *a, int cc, CcTarget target);
void gui_run_unbind(App *a, int cc); /* -1 = all */

/* widgets.c */
typedef enum { FADER_NONE, FADER_SET, FADER_RESET } FaderActKind;
typedef struct { FaderActKind kind; float t; } FaderAct;
float log_position(float p, float lo, float hi);
float position_of_log(float v, float lo, float hi);
FaderAct fader_track(Ui *ui, UiId id, Rct r, const char *label,
                     const char *value, float t);
bool fader(Ui *ui, UiId id, Rct r, const char *label, float *v, float lo,
           float hi);
bool fader_log(Ui *ui, UiId id, Rct r, const char *label, float *v, float lo,
               float hi, const char *suffix);
bool fader_int(Ui *ui, UiId id, Rct r, const char *label, int *v, int lo,
               int hi);
/* A rotary knob: label above, value below, the arc from 7:30 round to 4:30.
   Vertical drag sweeps the range in KNOB_DRAG_PX, Shift for a tenth of
   that; double-click resets. FADER_SET carries the new position. */
#define KNOB_DRAG_PX 150.0f
#define KNOB_FINE 0.1f
FaderAct knob_track(Ui *ui, UiId id, Rct r, const char *label,
                    const char *value, float t);
/* envelope times: `lo` at the bottom of the sweep, then equal ratios from
   ENV_TIME_FLOOR (or lo, if higher) up to ENV_TIME_MAX */
#define ENV_TIME_FLOOR 0.001f
float env_time_at(float t, float lo);
float env_time_pos(float v, float lo);
void hard_rect(Canvas *c, Rct r, float width);
void bubble_chain(Canvas *c, Ui *ui, P2 a, P2 b, bool active, float index,
                  double time);
void inverted_strip(Canvas *c, Rct r, const char *text);
Rct window_chrome_tagged(Canvas *c, Rct r, const char *title, const char *tag);
bool pane_button(Ui *ui, UiId id, Rct r, const char *text, bool armed);
bool chip_button(Ui *ui, UiId id, Rct r, const char *text, bool selected);
bool bookmark(Ui *ui, UiId id, Rct r, const char *label, bool selected);
int wave_tabs(Ui *ui, UiId id, Rct r, const char *const *labels, int n,
              int active);
void draw_graticule(Canvas *c, Rct r, int cols, int rows);
void beam_segment(Canvas *c, P2 a, P2 b, int k, bool decayed);
void dotted_rect(Canvas *c, Rct r, uint8_t ink);
extern const char *const ICON_COG[9];
void draw_icon(Canvas *c, const char *const rows[9], P2 at, uint8_t ink, float k);
bool icon_button(Ui *ui, UiId id, Rct r, const char *const rows[9],
                 const char *label, bool armed, float k);
float tab_width(float available, float gap, int n);
void draw_block_caret(Canvas *c, Ui *ui, FontId f, P2 text_pos,
                      const char *text, uint8_t bg);

/* presets.c (App-level flows; all log via push_log) */
void preset_rescan(App *a);
void preset_load(App *a, const PresetRef *r);
void preset_save_in(App *a, const char *bank, const char *name);
bool preset_run_save(App *a, const char *name);
void preset_run_overwrite(App *a, const char *args);
void preset_run_delete(App *a, const char *args);
void preset_run_rename(App *a, const char *args);
void preset_run_move(App *a, const char *args);
void preset_run_add(App *a, const char *args);
void preset_run_remove(App *a, const char *args);
void preset_cycle(App *a, bool forward);
Session app_session(const App *a);
void app_apply_session(App *a, Session s);
void app_save_state(App *a);   /* state.json on exit */
bool app_restore_state(App *a);

/* console.c */
void tell_new_tips(App *a);
void console_run_line(App *a, const char *line);
/* Tab highlights and cycles, Enter takes the highlight or runs the line,
   Escape backs out of the highlight (false when there was none) */
void console_tab(App *a);
void console_enter(App *a);
bool console_escape(App *a);
void draw_footer(App *a, Ui *ui, Rct r);
void draw_console_drawer(App *a, Ui *ui, Rct footer);

/* frame.c: one full UI frame onto ui->canvas; shared by the SDL shell and
   the plugin editor */
void app_frame(App *a, Ui *ui);
void app_init_defaults(App *a);

/* shared app helpers (centre.c) */
extern const char *const ROMAN[8];
const char *mode_name_of(RatioMode m);
int algorithm_index_of(const Patch *p);
void app_set_algorithm(App *a, int idx);
void app_set_engaged(App *a, bool on);

/* panes.c */
/* Tab walks search -> list -> buttons, Left/Right pick a bar button */
void presets_walk_keys(App *a, Ui *ui);
void draw_preset_bar(App *a, Ui *ui, Rct r);
void draw_presets_pane(App *a, Ui *ui);
void draw_info_pane(App *a, Ui *ui);
void draw_fps_counter(App *a, Ui *ui, float footer_h);

/* rails.c */
void draw_left_rail(App *a, Ui *ui, Rct r);
void draw_right_rail(App *a, Ui *ui, Rct r);

/* centre.c */
void draw_controls_house(App *a, Ui *ui, Rct r);
void draw_stage(App *a, Ui *ui, Rct r);    /* starfield + monolith */
void draw_display_cell(App *a, Ui *ui, Rct r);
void centre_prelayout(App *a, Ui *ui);     /* phase integrators, dread */

/* visuals.c: logalith + splash */
typedef struct {
    P2 pole;
    float tilt, max_r, bore_r, pitch, yaw, focal;
} Pose;
typedef struct {
    float suture_ph, ripple_ph, cycles, along, level, grown, index;
    uint32_t ghosts;
    float ghost_spread, stipple;
} Look;
void logalith_fit(Rct rect, P2 *pole, float *base_r);
void logalith_draw(Canvas *c, const Pose *pose, const Look *look, uint8_t ink);
float logalith_unit_r(float theta, float ph, float cycles);
bool splash_draw(App *a, Canvas *c, Rct rect, float elapsed);

#endif
