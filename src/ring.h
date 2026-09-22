#ifndef BYPO_RING_H
#define BYPO_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

/* single-producer single-consumer ring of fixed-size events.
   head and tail sit on their own cache lines. Each side remembers the
   other index and only reloads it when the ring looks empty or full, so a
   viz push does not bounce a line the GUI thread just wrote. Both caches
   start at 0 with the indices; the ring has to be zeroed. */
#define RING_DECLARE(name, type, cap)                                        \
    typedef struct {                                                         \
        type slots[cap];                                                     \
        _Alignas(64) _Atomic size_t head;                                    \
        size_t cached_tail; /* consumer only */                              \
        _Alignas(64) _Atomic size_t tail;                                    \
        size_t cached_head; /* producer only */                              \
    } name;                                                                  \
    static inline bool name##_push(name *r, type v) {                        \
        size_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);     \
        size_t next = (t + 1) % (cap);                                       \
        if (next == r->cached_head) {                                        \
            r->cached_head =                                                 \
                atomic_load_explicit(&r->head, memory_order_acquire);        \
            if (next == r->cached_head) return false;                        \
        }                                                                    \
        r->slots[t] = v;                                                     \
        atomic_store_explicit(&r->tail, next, memory_order_release);         \
        return true;                                                         \
    }                                                                        \
    static inline bool name##_pop(name *r, type *out) {                      \
        size_t h = atomic_load_explicit(&r->head, memory_order_relaxed);     \
        if (h == r->cached_tail) {                                           \
            r->cached_tail =                                                 \
                atomic_load_explicit(&r->tail, memory_order_acquire);        \
            if (h == r->cached_tail) return false;                           \
        }                                                                    \
        *out = r->slots[h];                                                  \
        atomic_store_explicit(&r->head, (h + 1) % (cap),                     \
                              memory_order_release);                         \
        return true;                                                         \
    }

#endif
