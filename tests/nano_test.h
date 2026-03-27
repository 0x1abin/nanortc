/*
 * nanortc — Shared test macros
 *
 * Include this header in every test_*.c file.
 * No external test framework dependency.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NANO_TEST_H_
#define NANO_TEST_H_

#include <stdio.h>

static int nano_tests_run = 0;
static int nano_tests_passed = 0;
static int nano_tests_failed = 0;

#define TEST(name)                                                                       \
    static void name(void);                                                              \
    static void run_##name(void)                                                         \
    {                                                                                    \
        nano_tests_run++;                                                                \
        printf("  %-50s", #name);                                                        \
        name();                                                                          \
        nano_tests_passed++;                                                             \
        printf(" OK\n");                                                                 \
    }                                                                                    \
    static void name(void)

#define ASSERT(cond)                                                                     \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            nano_tests_failed++;                                                         \
            nano_tests_passed--;                                                         \
            printf(" FAIL (%s:%d: %s)\n", __FILE__, __LINE__, #cond);                    \
            return;                                                                      \
        }                                                                                \
    } while (0)

#define ASSERT_EQ(a, b)    ASSERT((a) == (b))
#define ASSERT_NEQ(a, b)   ASSERT((a) != (b))
#define ASSERT_OK(expr)    ASSERT_EQ((expr), 0)
#define ASSERT_FAIL(expr)  ASSERT((expr) < 0)
#define ASSERT_TRUE(expr)  ASSERT((expr))
#define ASSERT_FALSE(expr) ASSERT(!(expr))

#define ASSERT_MEM_EQ(a, b, len) ASSERT(memcmp((a), (b), (len)) == 0)

#define TEST_MAIN_BEGIN(suite_name)                                                      \
    int main(void)                                                                       \
    {                                                                                    \
        printf("%s (DC=%d AUDIO=%d VIDEO=%d)\n", suite_name, \
               NANO_FEATURE_DATACHANNEL, NANO_FEATURE_AUDIO, NANO_FEATURE_VIDEO);

#define RUN(name) run_##name()

#define TEST_MAIN_END                                                                    \
        printf("\n%d/%d tests passed", nano_tests_passed, nano_tests_run);               \
        if (nano_tests_failed > 0) {                                                     \
            printf(" (%d FAILED)", nano_tests_failed);                                   \
        }                                                                                \
        printf("\n");                                                                     \
        return (nano_tests_failed == 0) ? 0 : 1;                                        \
    }

#endif /* NANO_TEST_H_ */
