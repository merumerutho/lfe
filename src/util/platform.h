/*
 * platform.h — Compile-time platform gates for the lfe library.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The library is portable C by default. Platform-specific optimizations
 * are opt-in via compile-time defines, falling back to portable C when
 * the platform isn't recognized. This file defines the gates that the
 * rest of the library uses to select between portable and optimized
 * code paths.
 *
 * The two recognized platforms today:
 *   LFE_PLATFORM_NDS_ARM9 — Nintendo DS ARM9 build (devkitARM toolchain)
 *   LFE_PLATFORM_HOST     — host build (system gcc, used for testing)
 *
 * Adding a new platform means defining its LFE_PLATFORM_* macro and
 * any helper macros (LFE_HOT, intrinsic wrappers, etc.).
 */

#ifndef LFE_UTIL_PLATFORM_H
#define LFE_UTIL_PLATFORM_H

/* ------------------------------------------------------------------ */
/* Detect the platform                                                 */
/* ------------------------------------------------------------------ */

#if defined(__arm__) && defined(__ARM_ARCH_5TE__)
#  define LFE_PLATFORM_NDS_ARM9 1
#else
#  define LFE_PLATFORM_HOST     1
#endif

/* ------------------------------------------------------------------ */
/* Hot-loop placement                                                  */
/*                                                                     */
/* On NDS ARM9, ITCM is a 16 KB block of single-cycle tightly-coupled  */
/* instruction memory. Functions marked LFE_HOT get placed there,      */
/* avoiding cache misses on the inner DSP loops. On host builds the    */
/* attribute is a no-op.                                                */
/* ------------------------------------------------------------------ */

#if LFE_PLATFORM_NDS_ARM9
#  define LFE_HOT __attribute__((section(".itcm"), long_call))
#else
#  define LFE_HOT
#endif

/* ------------------------------------------------------------------ */
/* Inlining hint                                                       */
/* ------------------------------------------------------------------ */

#if defined(__GNUC__)
#  define LFE_INLINE static inline __attribute__((always_inline))
#else
#  define LFE_INLINE static inline
#endif

/* ------------------------------------------------------------------ */
/* Unused-parameter suppression                                        */
/* ------------------------------------------------------------------ */

#define LFE_UNUSED(x) ((void)(x))

#endif /* LFE_UTIL_PLATFORM_H */
