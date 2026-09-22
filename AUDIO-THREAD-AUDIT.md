# Audio-thread audit: clicks on alt+tab

Date: 2026-09-22
Symptom: loud clicks and general roughness when alt+tabbing away from FL Studio
with bypo.clap open (Scarlett 2i2 4th Gen, driver 4.150). Closing the plugin
makes it stop, so this is the plugin, not the driver or Windows. The system
side was checked and is clean: High performance power plan, on AC, no USB or
driver errors in the event log, release build (`-O2`, no `-g`, no sanitizers).

## Verdict

The audio thread is clean in the classic sense: no mutex, no malloc/free, no
file I/O, no logging, no syscalls anywhere under `plug_process()`. GUI to audio
traffic goes through lock-free SPSC rings (`src/ring.h`) and relaxed atomics.
A lock-inversion stall is not the problem.

The real problem is that the plugin runs with almost no CPU headroom, and
alt+tab is the moment Windows piles compositor and scheduler work onto the
same cores. Three things combine, plus one alt+tab-specific bug that clicks on
its own.

## Ranked hazards

### 1. No FTZ/DAZ, and IIR feedback state is never flushed

`flush_tiny()` (`src/dsp/dsp.h:983`) is applied only to the final stereo
output (`src/plug.c:455`, `src/gui/gui_engine.c:248`). Nothing sets
`_MM_FLUSH_ZERO_MODE` / `_MM_DENORMALS_ZERO_MODE` anywhere in the repo, and
CLAP does not guarantee the host set it on the audio thread.

Every feedback loop decays toward zero but never reaches it:

- `comb_process` damping pole and delay write, `src/dsp/reverb.c:93-99`
- `ghost_process`, `allpass_process`, `svf_process`, `dc_block_process`,
  `src/dsp/reverb.c:105-330`
- chorus/chamber lines, `src/dsp/chandas.c:167-171`, `src/dsp/chamber.c:155-174`
- tape smoothers, `src/dsp/tape.c`

With decay up to 8 s and feedback near 1, a release tail sits in subnormal
range for many seconds. Without FTZ that is roughly 100+ cycles per op across
~10 combs, 5 ghosts, 4 allpasses, 4 SVFs and the chamber, per sample. This is
the most likely source of "random" dropouts right after notes stop, which is
exactly when you alt+tab away.

Fix: set FTZ+DAZ at the top of `plug_process()` (restore on exit), and add
`flush_tiny` or a `+1e-25f` bias to the comb/ghost/chamber feedback writes.

### 2. Transcendentals in the per-sample inner loop

Per output sample:

- `src/dsp/reverb.c:310` `powf`, `:312` `tanf`, `:313` → `verb_damp_q` →
  `src/dsp/reverb.c:37` another `powf`
- `src/dsp/reverb.c:86` `sinf` and `:98` `sqrtf` inside `comb_process`, x10
  combs, plus three integer `%` per comb (`:90-91`, `:100`)
- `src/dsp/voice.c:314` `powf` inside the NUM_OPS loop (up to 5 per
  sample per voice), `:316` a sixth `powf`, `:355` `sinf` x5
- `src/dsp/envelope.c:16` `powf` per `envelope_tick`, per sample via
  `src/dsp/voice.c:376`
- `src/dsp/chandas.c:167-171` `cosf` x2 + `fmodf` x2

With up to 8 live pairs (`BANK_PAIRS`, `src/dsp/dsp.h:368`) that is on the
order of 50-100 libm calls per output sample. `index`, `damp`, `fb`, `ratio`
only move at `PARAM_SMOOTH` rate. Hoist them to the `MOD_BLOCK`=32 boundary
(`src/dsp/dsp.h:840`) or use a table / `exp2` approximation.

### 3. Per-sample indirect call through `FrameEmit`

`voice_bank_render_frames(..., emit_frame, &e)` (`src/plug.c:449`, `:539`)
invokes a function pointer once per sample across TU boundaries
(`src/dsp/bank.c:362`, `src/dsp/voice.c:383`). The whole verb → chandas →
limiter chain sits behind it, so nothing inlines or vectorizes. Restructure to
block rendering: fill a `Frame[]` chunk, then process the chunk.

### 4. Full editor repaint at 62 Hz on the host message thread, no damage tracking

Canvas is 1180x780 XRGB = 3.68 MB (`src/gui/app.h:20-21`, `src/gui/canvas.c:19`).
Every tick, unconditionally:

- `canvas_fill(c, INK_BLACK)` writes all 920 400 pixels, `src/gui/frame.c:54`
- the entire UI is re-rasterized, `src/gui/frame.c:65-97`
- `canvas_fingerprint()` reads the whole canvas, `src/plug_gui.c:238-252`
- `magnify()` reads 3.68 MB and writes scale² x 3.68 MB, `src/plug_gui.c:216-232`
- `StretchDIBits` of the whole window, `src/plug_gui_win32.c:77`

At display scale 2.0-2.5 (`src/gui/scale.c:11-20`) the output buffer is
15-23 MB and the plugin moves 2-3 GB/s of memory at 62 fps. The hash only
saves the blit; `app_frame` has already done all the work by the time the
early-out at `src/plug_gui.c:304` fires. This evicts the audio thread's delay
lines from L2/L3 continuously. Backend is pure GDI software blit, no GL/D3D,
no vsync, so no compositor wait but no GPU offload either.

Fixes: drop the timer to 30 Hz when nothing animates; skip
`canvas_fingerprint` and use the existing `ui->repaint_soon` dirty flag
(`src/gui/ui.h:39`); fold `magnify` into `StretchDIBits` and let GDI do the
nearest-neighbour scale from the 1180x780 DIB.

### 5. `apply_vals()` runs a full engine rebuild per parameter event

Every `CLAP_EVENT_PARAM_VALUE` calls `apply_vals()` (`src/plug.c:147-204`,
called at `:587`), which does:

- `voice_bank_set_patch` → `voice_bank_set_state` over all 8 pairs x 2 voices,
  each `voice_apply_patch` → `compile()` (`src/dsp/bank.c:129-155`,
  `src/dsp/voice.c:232`, `src/dsp/algorithm.c:152`)
- `verb_configure` → 4 x NUM_OPS `sqrtf` + `powf` via `comb_fb_for`
  (`src/dsp/reverb.c:224-243`, `:62`)
- full chandas/melody/limiter/tape reconfigure

That is ~16 algorithm recompiles and ~20 `powf` per param event. Dragging a
fader while the host echoes values, or any automation lane, fires this dozens
of times per block. `params_flush` does the same (`src/plug.c:794`). Coalesce:
set `dirty` and apply once per block. The `dirty` path at `src/plug.c:634`
already does this; the per-event call is the redundant one.

### 6. Ring head/tail share a cache line

`src/ring.h:12`: `slots[cap]` followed by `_Atomic size_t head, tail;` on the
same line. `VizRing_push` does an acquire load of `head` on every push
(`src/plug.c:472`) at sr/4 ≈ 12 kHz. While recording, `RecRing_push` runs
twice per sample (`src/plug.c:476-477`). Every `*_pop` on the GUI thread
(`src/gui/gui_engine.c:489`, `:744`) invalidates that line for the audio
thread. Pad `head` and `tail` to separate 64-byte lines and cache the opposite
index locally.

### 7. Window procedure ignores focus and capture loss (alt+tab specific)

`src/plug_gui_win32.c:132-201` has no `WM_ACTIVATE`, `WM_ACTIVATEAPP`,
`WM_SETFOCUS`, `WM_KILLFOCUS`, `WM_CAPTURECHANGED` or `WM_DPICHANGED` case.
Nothing heavy runs on focus change, which is good. Two real bugs though:

- Stuck Tab key. Alt+Tab delivers `WM_KEYDOWN(VK_TAB)` →
  `gui_in_key(g, KEY_TAB, true)` (`:184`, `src/plug_gui.c:162-171`); the
  matching `WM_KEYUP` goes to the other window, so
  `pending.key_down[KEY_TAB]` stays latched.
- Stuck mouse capture. `WM_LBUTTONDOWN` calls `SetCapture` (`:161`). Focus
  stolen mid-drag means no `WM_LBUTTONUP` and no capture-changed handler, so
  `pending.down` stays true. On return, the first mouse move jumps the value
  in one step. Host param writes have no smoothing (`plug_setv` is a bare
  atomic store, `src/plug.c:83-89`), so that is an audible click by itself.

Fix: add `WM_CAPTURECHANGED` / `WM_KILLFOCUS` handlers that clear
`pending.down`, `key_down[]` and `mouse_in_window`.

### 8. Two 16 ms timers registered at once

The host CLAP timer is registered in `gui_create` (`src/plug_gui.c:426`,
`TIMER_MS 16`) and the native Win32 `SetTimer` starts in `gui_set_parent`
(`src/plug_gui.c:529` → `src/plug_gui_win32.c:332`, `FRAME_TIMER_MS 16`).
`on_timer` bails when `g->native_timer` is set (`src/plug_gui.c:587`) so there
is no double render, but the host main thread services two timers forever.
Unregister the host timer once the native one takes over. The render also
runs inside the window procedure (`src/plug_gui_win32.c:146`), so a slow frame
blocks the host's whole message pump.

### 9. Editor open does directory scanning and file writes on the main thread

`src/plug_gui.c:375-394`: `prepare_preset_dir()` (`src/gui/presets.c:762`)
walks the stock bank with `opendir`/`readdir`, copies files and writes a
ledger (`src/gui/presets.c:684`, `:751`, `:807`); `text_init()` loads FreeType
and two faces; `preset_rescan()` scans the tree again
(`src/gui/presets.c:1022`). All synchronous on the host main thread. Expect a
one-shot dropout on editor open, not on alt+tab.

### 10. Non-atomic cross-thread fields (correctness, not clicks)

`p->mel_sync` / `p->mel_division` are written by the audio thread
(`src/plug.c:361-362`) and by the main thread in `state_load`
(`src/plug.c:858-859`), read by the audio thread in `apply_vals`
(`src/plug.c:177-178`) and by the GUI via `plug_session_of_vals` →
`refresh_shadows` (`src/plug_gui.c:112-127`, `src/plug.c:223-224`). Same for
`p->pitch_main` / `p->mods_main` and `g->app->mods` in `state_save`
(`src/plug.c:808-809`). `state_load` also pushes `1 + SEQS + MOD_ROUTES`
events into a 256-slot ring (`src/plug.c:863-869`) with no overflow check.

### 11. Voice/note handling: clean

No allocation on note on/off; all buffers are sized at max in `*_init`
during `plug_activate` (`src/plug.c:893-917`). No sample loading or preset
load on the audio thread. `chandas_clear()` (`src/dsp/chandas.c:256-268`)
loops the whole buffer; it is not reachable from `chandas_set_params` today.

### 12. Threads: clean

Only two `pthread_create` sites, both standalone-only and not linked into
bypo.clap (`src/audio.c:56`, `src/midi.c:174`). No priority calls.

## Build config

Release build. `Makefile:2` → `-std=gnu11 -O2 -Wall -Wextra`. No `-g`, `-O0`,
sanitizers or `-ffast-math`. The 18 MB artifact is `.rdata` from `-static`
(FreeType, HarfBuzz, libstdc++, zlib/png/brotli) plus the embedded fonts.

Two build-side wins:

- `-O2` with no `-march` means baseline SSE2. `-ffp-contract=fast` plus a
  tuned `-march` (or a runtime-dispatched hot path) is worth real percentage
  points given hazard 2.
- Keep `-ffast-math` off, but note `powf`/`sinf` are full-precision libm calls
  with no vectorized variants.

## Suggested order

1. FTZ/DAZ in `plug_process()` plus feedback-path flushing (hazard 1). Small.
2. `WM_KILLFOCUS` / `WM_CAPTURECHANGED` handlers (hazard 7). Small, targets
   alt+tab directly.
3. Repaint: 30 Hz idle, dirty flag instead of fingerprint, GDI upscale
   (hazard 4).
4. Hoist per-sample `powf`/`sinf` to block boundary (hazard 2).
5. Coalesce `apply_vals` per block (hazard 5), pad ring indices (hazard 6).
