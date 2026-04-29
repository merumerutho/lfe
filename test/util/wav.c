/*
 * wav.c — Mono 16-bit PCM WAV writer.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Standard RIFF/WAVE format. The header is 44 bytes:
 *
 *   "RIFF"   4 bytes
 *   size     4 bytes (file_size - 8)
 *   "WAVE"   4 bytes
 *   "fmt "   4 bytes
 *   16       4 bytes (fmt chunk size for PCM)
 *   1        2 bytes (audio format = PCM)
 *   1        2 bytes (channels = 1, mono)
 *   rate     4 bytes
 *   byte_rate 4 bytes (rate * channels * bits/8)
 *   2        2 bytes (block align = channels * bits/8)
 *   16       2 bytes (bits per sample)
 *   "data"   4 bytes
 *   size     4 bytes (data chunk size = length * 2)
 *
 * Followed by `length` int16 samples written verbatim.
 */

#include "wav.h"

#include <stdio.h>
#include <string.h>

static int write_u32_le(FILE *f, uint32_t v)
{
    uint8_t buf[4];
    buf[0] = (uint8_t)( v        & 0xFF);
    buf[1] = (uint8_t)((v >>  8) & 0xFF);
    buf[2] = (uint8_t)((v >> 16) & 0xFF);
    buf[3] = (uint8_t)((v >> 24) & 0xFF);
    return fwrite(buf, 1, 4, f) == 4 ? 0 : -1;
}

static int write_u16_le(FILE *f, uint16_t v)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)( v       & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
    return fwrite(buf, 1, 2, f) == 2 ? 0 : -1;
}

int lfe_test_wav_write_mono16(const char *path,
                              const int16_t *samples,
                              uint32_t length,
                              uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    const uint32_t data_bytes = length * 2u;
    const uint32_t riff_size  = 36u + data_bytes;

    if (fwrite("RIFF", 1, 4, f) != 4) goto fail;
    if (write_u32_le(f, riff_size))    goto fail;
    if (fwrite("WAVE", 1, 4, f) != 4) goto fail;

    if (fwrite("fmt ", 1, 4, f) != 4) goto fail;
    if (write_u32_le(f, 16u))          goto fail;  /* fmt chunk size */
    if (write_u16_le(f, 1u))           goto fail;  /* PCM */
    if (write_u16_le(f, 1u))           goto fail;  /* mono */
    if (write_u32_le(f, rate))         goto fail;
    if (write_u32_le(f, rate * 2u))    goto fail;  /* byte rate */
    if (write_u16_le(f, 2u))           goto fail;  /* block align */
    if (write_u16_le(f, 16u))          goto fail;  /* bits per sample */

    if (fwrite("data", 1, 4, f) != 4) goto fail;
    if (write_u32_le(f, data_bytes))   goto fail;

    /* Sample data — write the int16 buffer verbatim. Assumes
     * little-endian host. */
    if (fwrite(samples, sizeof(int16_t), length, f) != length)
        goto fail;

    fclose(f);
    return 0;

fail:
    fclose(f);
    return -2;
}
