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

/* MinGW: mkdir takes no mode and lives in <direct.h>; forward slashes in
   paths are fine everywhere else in the Win32 file API. */
#if defined(_WIN32)
#include <direct.h>
#include <stdlib.h>
#include <sys/stat.h>
#define mkdir(path, mode) _mkdir(path)
/* realpath -> _fullpath; both take/return a caller buffer of PATH_MAX-ish
   size (every caller in this tree passes a 1024-byte buffer) */
#define realpath(path, resolved) _fullpath((resolved), (path), 1024)
#endif

#endif
