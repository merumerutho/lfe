/*
 * fx_reverse.c — Reverse the sample order within a selection.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fx_common.h"

lfe_status lfe_fx_reverse(lfe_buffer *buf, const lfe_fx_range *range)
{
    uint32_t count;
    lfe_status rc = lfe_fx_validate_range(buf, range, &count);
    if (rc != LFE_OK) return rc;
    if (count <= 1) return LFE_OK;

    lfe_sample_t *data = buf->data;
    uint32_t lo = range->start;
    uint32_t hi = range->end - 1;

    while (lo < hi) {
        lfe_sample_t tmp = data[lo];
        data[lo] = data[hi];
        data[hi] = tmp;
        lo++;
        hi--;
    }

    return LFE_OK;
}
