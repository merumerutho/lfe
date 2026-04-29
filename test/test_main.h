/*
 * test_main.h — Shared assert macros and counters for the lfe test runner.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Modeled on maxtracker's existing test.c style: file-static counters,
 * MT_ASSERT-style macros that print and increment, no setjmp/longjmp.
 * Each test function asserts inline; the runner calls them in order.
 */

#ifndef LFE_TEST_MAIN_H
#define LFE_TEST_MAIN_H

#include <stdio.h>
#include <stdint.h>

extern int g_lfe_test_passed;
extern int g_lfe_test_failed;

#define LFE_TEST_ASSERT(cond, msg) do {                              \
    if (cond) {                                                       \
        g_lfe_test_passed++;                                          \
    } else {                                                          \
        g_lfe_test_failed++;                                          \
        fprintf(stderr, "FAIL: %s:%d %s\n", __FILE__, __LINE__, msg); \
    }                                                                 \
} while (0)

#define LFE_TEST_ASSERT_EQ(actual, expected, msg) do {                \
    long long _a = (long long)(actual);                               \
    long long _e = (long long)(expected);                             \
    if (_a == _e) {                                                   \
        g_lfe_test_passed++;                                          \
    } else {                                                          \
        g_lfe_test_failed++;                                          \
        fprintf(stderr, "FAIL: %s:%d %s (got %lld, expected %lld)\n", \
                __FILE__, __LINE__, msg, _a, _e);                     \
    }                                                                 \
} while (0)

#define LFE_TEST_HEADER(name) printf("-- %s --\n", name)

/* Test entry points — declared here, defined in their own .c files. */
void lfe_test_test_tone(void);
void lfe_test_envelope(void);
void lfe_test_noise(void);
void lfe_test_filter(void);
void lfe_test_drawn(void);
void lfe_test_drum(void);
void lfe_test_synth(void);
void lfe_test_fm4(void);
void lfe_test_fx(void);
void lfe_test_ott(void);
void lfe_test_braids(void);
void lfe_test_dbmath(void);
void lfe_test_lfo(void);

#endif /* LFE_TEST_MAIN_H */
