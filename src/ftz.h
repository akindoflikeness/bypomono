#ifndef BYPO_FTZ_H
#define BYPO_FTZ_H

#include <stdint.h>

/* Flush denormals to zero for one audio callback, then put the control
   register back. Hosts do not promise this, and a release tail that has
   decayed into subnormals is slow enough to click. */

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include <pmmintrin.h>
typedef unsigned ftz_state;
static inline ftz_state ftz_begin(void) {
    unsigned mx = _mm_getcsr();
    _mm_setcsr(mx | 0x8040u); /* FTZ (bit 15) and DAZ (bit 6) */
    return mx;
}
static inline void ftz_end(ftz_state mx) { _mm_setcsr(mx); }
#elif defined(__aarch64__)
typedef uint64_t ftz_state;
static inline ftz_state ftz_begin(void) {
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ volatile("msr fpcr, %0" ::"r"(fpcr | (1ull << 24)) : "memory");
    return fpcr;
}
static inline void ftz_end(ftz_state fpcr) {
    __asm__ volatile("msr fpcr, %0" ::"r"(fpcr) : "memory");
}
#else
typedef int ftz_state;
static inline ftz_state ftz_begin(void) { return 0; }
static inline void ftz_end(ftz_state s) { (void)s; }
#endif

#endif
