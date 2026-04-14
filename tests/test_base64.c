/*
 * nanortc — Base64 encoder tests (RFC 4648)
 *
 * Test vectors from RFC 4648 §10 "Test Vectors".
 * The RFC provides exactly 7 canonical vectors covering every tail length
 * (0, 1, 2 bytes) and padding case.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_base64.h"
#include "nano_test.h"
#include "nanortc.h"
#include <string.h>

/* ================================================================
 * RFC 4648 §10 canonical vectors
 * ================================================================ */

static void check_vector(const char *plain, const char *expected)
{
    size_t plain_len = 0;
    while (plain[plain_len] != '\0') {
        plain_len++;
    }
    size_t expected_len = 0;
    while (expected[expected_len] != '\0') {
        expected_len++;
    }

    char out[64];
    size_t out_len = 0;
    ASSERT_OK(nano_base64_encode((const uint8_t *)plain, plain_len, out, sizeof(out), &out_len));
    ASSERT_EQ(out_len, expected_len);
    if (expected_len > 0) {
        /* Unity's TEST_ASSERT_EQUAL_MEMORY refuses zero-length compares. */
        ASSERT_MEM_EQ(out, expected, expected_len);
    }
    ASSERT_EQ(out[out_len], '\0'); /* NUL terminator */
}

TEST(test_base64_rfc4648_empty)
{
    check_vector("", "");
}

TEST(test_base64_rfc4648_f)
{
    check_vector("f", "Zg==");
}

TEST(test_base64_rfc4648_fo)
{
    check_vector("fo", "Zm8=");
}

TEST(test_base64_rfc4648_foo)
{
    check_vector("foo", "Zm9v");
}

TEST(test_base64_rfc4648_foob)
{
    check_vector("foob", "Zm9vYg==");
}

TEST(test_base64_rfc4648_fooba)
{
    check_vector("fooba", "Zm9vYmE=");
}

TEST(test_base64_rfc4648_foobar)
{
    check_vector("foobar", "Zm9vYmFy");
}

/* ================================================================
 * Additional coverage (non-RFC, implementation-specific)
 * ================================================================ */

/* Exercise all 64 alphabet characters. RFC 4648 §4 alphabet:
 *   index 0..25 = A..Z, 26..51 = a..z, 52..61 = 0..9, 62 = +, 63 = /
 * The bytes 0x00..0xFF contain each 6-bit value at least once; we use
 * a crafted 48-byte input that maps to every alphabet index in order. */
TEST(test_base64_all_alphabet)
{
    /* 48 input bytes → 64 output characters, hitting every base64 index.
     * Source: derived independently from RFC 4648 §4 alphabet table. */
    static const uint8_t src[48] = {
        0x00, 0x10, 0x83, 0x10, 0x51, 0x87, 0x20, 0x92, 0x8b, 0x30, 0xd3, 0x8f,
        0x41, 0x14, 0x93, 0x51, 0x55, 0x97, 0x61, 0x96, 0x9b, 0x71, 0xd7, 0x9f,
        0x82, 0x18, 0xa3, 0x92, 0x59, 0xa7, 0xa2, 0x9a, 0xab, 0xb2, 0xdb, 0xaf,
        0xc3, 0x1c, 0xb3, 0xd3, 0x5d, 0xb7, 0xe3, 0x9e, 0xbb, 0xf3, 0xdf, 0xbf,
    };
    static const char expected[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    char out[96];
    size_t out_len = 0;
    ASSERT_OK(nano_base64_encode(src, sizeof(src), out, sizeof(out), &out_len));
    ASSERT_EQ(out_len, 64);
    ASSERT_MEM_EQ(out, expected, 64);
}

/* Buffer-too-small returns error without touching out_len. */
TEST(test_base64_buffer_too_small)
{
    char out[4];
    size_t out_len = 0;
    /* "foobar" needs 9 bytes (8 chars + NUL); only 4 available → error. */
    int rc = nano_base64_encode((const uint8_t *)"foobar", 6, out, sizeof(out), &out_len);
    ASSERT_EQ(rc, NANORTC_ERR_BUFFER_TOO_SMALL);
    ASSERT_EQ(out_len, 0);
}

/* NULL parameter validation. */
TEST(test_base64_null_params)
{
    char out[16];
    size_t out_len = 0;
    ASSERT_FAIL(nano_base64_encode(NULL, 4, out, sizeof(out), &out_len));
    ASSERT_FAIL(nano_base64_encode((const uint8_t *)"f", 1, NULL, sizeof(out), &out_len));
    ASSERT_FAIL(nano_base64_encode((const uint8_t *)"f", 1, out, sizeof(out), NULL));
}

/* Empty input with non-NULL src and tiny output buffer: must NUL-terminate. */
TEST(test_base64_empty_with_dst)
{
    char out[1];
    size_t out_len = 0;
    ASSERT_OK(nano_base64_encode((const uint8_t *)"", 0, out, sizeof(out), &out_len));
    ASSERT_EQ(out_len, 0);
    ASSERT_EQ(out[0], '\0');
}

/* Encoded size calculation helper matches the encoder output length. */
TEST(test_base64_encoded_size_helper)
{
    /* 0 → 1 (just NUL), 1 → 5 ("X===\0"), 2 → 5, 3 → 5, 4 → 9, 5 → 9, 6 → 9, 7 → 13 */
    ASSERT_EQ(nano_base64_encoded_size(0), 1u);
    ASSERT_EQ(nano_base64_encoded_size(1), 5u);
    ASSERT_EQ(nano_base64_encoded_size(2), 5u);
    ASSERT_EQ(nano_base64_encoded_size(3), 5u);
    ASSERT_EQ(nano_base64_encoded_size(4), 9u);
    ASSERT_EQ(nano_base64_encoded_size(5), 9u);
    ASSERT_EQ(nano_base64_encoded_size(6), 9u);
    ASSERT_EQ(nano_base64_encoded_size(7), 13u);
}

/* ================================================================
 * Test runner
 * ================================================================ */

TEST_MAIN_BEGIN("test_base64")
/* RFC 4648 §10 canonical vectors */
RUN(test_base64_rfc4648_empty);
RUN(test_base64_rfc4648_f);
RUN(test_base64_rfc4648_fo);
RUN(test_base64_rfc4648_foo);
RUN(test_base64_rfc4648_foob);
RUN(test_base64_rfc4648_fooba);
RUN(test_base64_rfc4648_foobar);
/* Alphabet coverage + edge cases */
RUN(test_base64_all_alphabet);
RUN(test_base64_buffer_too_small);
RUN(test_base64_null_params);
RUN(test_base64_empty_with_dst);
RUN(test_base64_encoded_size_helper);
TEST_MAIN_END
