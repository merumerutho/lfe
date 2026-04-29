/*
 * wav.h — Minimal mono 16-bit PCM WAV writer for lfe tests.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Used by the test harness to dump generator outputs to disk so they
 * can be ear-tested by humans (or hash-compared by future regression
 * tests). Not part of the library itself — lives in the test tree.
 */

#ifndef LFE_TEST_WAV_H
#define LFE_TEST_WAV_H

#include <stdint.h>

/*
 * Write a mono 16-bit signed-LE PCM WAV file at `path`.
 *
 *   path    — destination filename, opened with fopen(..., "wb")
 *   samples — int16_t buffer
 *   length  — number of samples (NOT bytes)
 *   rate    — sample rate in Hz
 *
 * Returns 0 on success, -1 on file open failure, -2 on write failure.
 *
 * Assumes a little-endian host. NDS hosts and most modern desktops are
 * little-endian. If we ever need big-endian support, we'll byteswap.
 */
int lfe_test_wav_write_mono16(const char *path,
                              const int16_t *samples,
                              uint32_t length,
                              uint32_t rate);

#endif /* LFE_TEST_WAV_H */
