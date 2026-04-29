/*
 * lfe.c — Library entry points (init, shutdown, version).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "lfe.h"

#include "util/wavetable.h"

#define LFE_VERSION_STRING "0.1.0"

static bool g_lfe_initialized = false;

lfe_status lfe_init(void)
{
    if (g_lfe_initialized)
        return LFE_OK;

    lfe_wavetable_init();

    g_lfe_initialized = true;
    return LFE_OK;
}

lfe_status lfe_shutdown(void)
{
    g_lfe_initialized = false;
    return LFE_OK;
}

const char *lfe_version(void)
{
    return LFE_VERSION_STRING;
}

/* Internal accessor used by generators to bail when the library
 * hasn't been initialized. Not exposed in lfe.h. */
bool lfe_is_initialized(void)
{
    return g_lfe_initialized;
}

/* ---- Generator registry ---- */

static const lfe_gen_info g_lfe_gen_table[] = {
#define X(id, name, short_name, desc) \
    { LFE_GEN_##id, name, short_name, desc },
    LFE_GEN_LIST(X)
#undef X
};

const lfe_gen_info *lfe_gen_lookup(lfe_gen_id id)
{
    if ((int)id < 0 || (int)id >= LFE_GEN_COUNT) return 0;
    return &g_lfe_gen_table[(int)id];
}

const char *lfe_gen_name(lfe_gen_id id)
{
    const lfe_gen_info *g = lfe_gen_lookup(id);
    return g ? g->name : "?";
}

const char *lfe_gen_short_name(lfe_gen_id id)
{
    const lfe_gen_info *g = lfe_gen_lookup(id);
    return g ? g->short_name : "?";
}

const char *lfe_gen_description(lfe_gen_id id)
{
    const lfe_gen_info *g = lfe_gen_lookup(id);
    return g ? g->description : "";
}
