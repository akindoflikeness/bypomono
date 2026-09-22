#include "app.h"

/* Repeated controls (fader drags, CC, a preset's fields) used to queue one
   event each. The host often stops calling process while a plugin is silent,
   so a few seconds of dragging filled the 256-slot ring and the console
   reported the change dropped. Those controls keep their latest value here.
   Notes, panic and chandas reset stay in order on the ring. */

static CtrlBox *box_for(App *a, const Event *ev) {
    switch (ev->kind) {
    case EV_SET_PATCH: return &a->ctrl_box[BOX_PATCH];
    case EV_SET_VERB: return &a->ctrl_box[BOX_VERB];
    case EV_SET_MELODY: return &a->ctrl_box[BOX_MELODY];
    case EV_SET_CHANDAS: return &a->ctrl_box[BOX_CHANDAS];
    case EV_SET_WARMTH: return &a->ctrl_box[BOX_WARMTH];
    case EV_SET_LIMITER: return &a->ctrl_box[BOX_LIMITER];
    case EV_SET_TEMPO: return &a->ctrl_box[BOX_TEMPO];
    case EV_GLIDE_TO: return &a->ctrl_box[BOX_GLIDE];
    case EV_ENGAGE: return &a->ctrl_box[BOX_ENGAGE];
    case EV_BEND: return &a->ctrl_box[BOX_BEND];
    case EV_SET_ADSR: return &a->ctrl_box[BOX_ADSR];
    case EV_SET_MIDI_DRIVING: return &a->ctrl_box[BOX_MIDI];
    case EV_SET_TRANSPORT: return &a->ctrl_box[BOX_TRANSPORT];
    case EV_RECORD: return &a->ctrl_box[BOX_RECORD];
    case EV_SET_PITCH: return &a->ctrl_box[BOX_PITCH];
    case EV_SET_SEQ:
        if (ev->u.seq.slot < 0 || ev->u.seq.slot >= SEQS) return NULL;
        return &a->seq_box[ev->u.seq.slot];
    case EV_SET_ROUTE:
        if (ev->u.route.slot < 0 || ev->u.route.slot >= MOD_ROUTES) return NULL;
        return &a->route_box[ev->u.route.slot];
    default:
        return NULL;
    }
}

static void box_publish(CtrlBox *b, Event ev) {
    uint32_t s = atomic_load_explicit(&b->seq, memory_order_relaxed);
    atomic_store_explicit(&b->seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    b->ev = ev;
    atomic_store_explicit(&b->seq, s + 2, memory_order_release);
}

static bool box_take(CtrlBox *b, uint32_t *seen, Event *out) {
    for (int spin = 0; spin < 8; spin++) {
        uint32_t s = atomic_load_explicit(&b->seq, memory_order_acquire);
        if (s & 1u) continue;
        if (s == 0 || s == *seen) return false;
        Event ev = b->ev;
        atomic_thread_fence(memory_order_acquire);
        uint32_t after = atomic_load_explicit(&b->seq, memory_order_relaxed);
        if (s != after) continue;
        *out = ev;
        *seen = s;
        return true;
    }
    return false;
}

void app_send(App *a, Event ev) {
    CtrlBox *b = box_for(a, &ev);
    if (b) {
        box_publish(b, ev);
        return;
    }
    if (!EventRing_push(&a->ctrl, ev)) {
        if (!a->ctrl_drop_logged) {
            push_log(a, "control ring full — a change was dropped.");
            a->ctrl_drop_logged = true;
        }
        return;
    }
    a->ctrl_drop_logged = false;
}

void app_drain_ctrl(App *a, void (*apply)(void *ud, Event ev), void *ud) {
    Event ev;
    for (int i = 0; i < BOX_COUNT; i++)
        if (box_take(&a->ctrl_box[i], &a->ctrl_seen[i], &ev)) apply(ud, ev);
    for (int i = 0; i < SEQS; i++)
        if (box_take(&a->seq_box[i], &a->seq_seen[i], &ev)) apply(ud, ev);
    for (int i = 0; i < MOD_ROUTES; i++)
        if (box_take(&a->route_box[i], &a->route_seen[i], &ev)) apply(ud, ev);
    while (EventRing_pop(&a->ctrl, &ev)) apply(ud, ev);
}
