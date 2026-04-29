/*
 * test_main.c — Host test runner entry point.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "test_main.h"

#include "lfe.h"

int g_lfe_test_passed = 0;
int g_lfe_test_failed = 0;

int main(void)
{
    printf("=== lfe library tests (version %s) ===\n\n", lfe_version());

    if (lfe_init() != LFE_OK) {
        fprintf(stderr, "lfe_init failed\n");
        return 2;
    }

    lfe_test_test_tone();
    lfe_test_envelope();
    lfe_test_lfo();
    lfe_test_noise();
    lfe_test_filter();
    lfe_test_drawn();
    lfe_test_drum();
    lfe_test_synth();
    lfe_test_fm4();
    lfe_test_fx();
    lfe_test_ott();
    lfe_test_braids();
    lfe_test_dbmath();

    printf("\n=== Results: %d/%d passed",
           g_lfe_test_passed,
           g_lfe_test_passed + g_lfe_test_failed);
    if (g_lfe_test_failed > 0)
        printf(", %d FAILED", g_lfe_test_failed);
    printf(" ===\n");

    lfe_shutdown();
    return g_lfe_test_failed > 0 ? 1 : 0;
}
