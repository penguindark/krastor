/* Hand-written stand-in for autotools' config.h when building SpeexDSP inside
 * Krastor (see libs/audiofx/CMakeLists.txt). Floating-point build with the
 * portable smallft FFT - same shape as the upstream win32/config.h, but also
 * valid for MinGW. */
#ifndef KRASTOR_SPEEXDSP_CONFIG_H
#define KRASTOR_SPEEXDSP_CONFIG_H

#if defined(_MSC_VER)
#define inline __inline
#endif

#define FLOATING_POINT
#define USE_SMALLFT

/* No visibility attributes on Windows/MSVC. */
#define EXPORT

#endif /* KRASTOR_SPEEXDSP_CONFIG_H */
