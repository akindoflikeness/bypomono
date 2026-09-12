#ifndef BYPO_COMPAT_H
#define BYPO_COMPAT_H

/* glibc 2.38/2.43 re-versioned these ancient libm entry points; pin to the
   originals so the binaries run inside Flatpak runtimes and older distros */
#if defined(__x86_64__) && defined(__gnu_linux__) && !defined(__clang__)
__asm__(".symver fmodf,fmodf@GLIBC_2.2.5");
__asm__(".symver atan2f,atan2f@GLIBC_2.2.5");
__asm__(".symver log10f,log10f@GLIBC_2.2.5");
__asm__(".symver __isoc23_strtol,strtol@GLIBC_2.2.5");
#endif

#endif
