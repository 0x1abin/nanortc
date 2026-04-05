/*
 * nanortc — Test compatibility layer (backed by Unity)
 *
 * Include this header in every test_*.c file.
 * Provides the same API as the original nano_test.h macros,
 * backed by the Unity test framework for better diagnostics.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANORTC_TEST_H_
#define NANORTC_TEST_H_

#include "unity.h"
#include <stdio.h>

/* Unity requires setUp/tearDown — provide empty defaults.
 * Each test executable is built from a single .c file,
 * so there is exactly one definition per binary. */
void setUp(void) {}
void tearDown(void) {}

/* ---- Test declaration ----
 * TEST(name) declares a void function.
 * The original macro also created a run_##name wrapper with
 * print/pass tracking — Unity handles that internally now. */
#define TEST(name) void name(void)

/* ---- Assertions ----
 * Map to Unity assertions. Using TEST_ASSERT for generic comparisons
 * to support all types (int, uint32_t, pointers, etc.) without
 * narrowing conversions. */
#define ASSERT(cond)           TEST_ASSERT(cond)
#define ASSERT_EQ(a, b)        TEST_ASSERT_TRUE_MESSAGE((a) == (b), #a " == " #b)
#define ASSERT_NEQ(a, b)       TEST_ASSERT_TRUE_MESSAGE((a) != (b), #a " != " #b)
#define ASSERT_OK(expr)        TEST_ASSERT_EQUAL_INT_MESSAGE(0, (expr), #expr " == 0")
#define ASSERT_FAIL(expr)      TEST_ASSERT_LESS_THAN_INT_MESSAGE(0, (expr), #expr " < 0")
#define ASSERT_TRUE(expr)      TEST_ASSERT_TRUE(expr)
#define ASSERT_FALSE(expr)     TEST_ASSERT_FALSE(expr)
#define ASSERT_MEM_EQ(a, b, len) TEST_ASSERT_EQUAL_MEMORY((a), (b), (len))

/* ---- Test runner ----
 * TEST_MAIN_BEGIN/END wrap main() with Unity lifecycle.
 * Feature flag banner is preserved for CI log readability. */
#define TEST_MAIN_BEGIN(suite_name)                                                          \
    int main(void)                                                                           \
    {                                                                                        \
        printf("%s (DC=%d AUDIO=%d VIDEO=%d)\n", suite_name,                                 \
               NANORTC_FEATURE_DATACHANNEL, NANORTC_FEATURE_AUDIO, NANORTC_FEATURE_VIDEO);   \
        UNITY_BEGIN();

#define RUN(name) RUN_TEST(name)

#define TEST_MAIN_END                                                                        \
        return UNITY_END();                                                                  \
    }

#endif /* NANORTC_TEST_H_ */
