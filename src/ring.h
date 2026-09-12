#ifndef BYPO_RING_H
#define BYPO_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

/* single-producer single-consumer ring of fixed-size events */
#define RING_DECLARE(name, type, cap)                                        \
    typedef struct {                                                         \
        type slots[cap];                                                     \
        _Atomic size_t head, tail;                                           \
    } name;                                                                  \
    static inline bool name##_push(name *r, type v) {                        \
        size_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);     \
        size_t next = (t + 1) % (cap);                                       \
        if (next == atomic_load_explicit(&r->head, memory_order_acquire))    \
            return false;                                                    \
        r->slots[t] = v;                                                     \
        atomic_store_explicit(&r->tail, next, memory_order_release);         \
        return true;                                                         \
    }                                                                        \
    static inline bool name##_pop(name *r, type *out) {                      \
        size_t h = atomic_load_explicit(&r->head, memory_order_relaxed);     \
        if (h == atomic_load_explicit(&r->tail, memory_order_acquire))       \
            return false;                                                    \
        *out = r->slots[h];                                                  \
        atomic_store_explicit(&r->head, (h + 1) % (cap),                     \
                              memory_order_release);                         \
        return true;                                                         \
    }

#endif
