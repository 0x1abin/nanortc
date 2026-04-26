/*
 * nanortc — CRC-32 (ISO HDLC) and CRC-32c (Castagnoli) unit tests
 *
 * The CRC modules carry 100% coverage indirectly via fuzz_stun
 * (FINGERPRINT verify) and fuzz_sctp (chunk checksum). This file pins
 * the algorithm itself to the canonical RFC test vectors so an
 * accidental polynomial / initial-value / final-XOR change is caught
 * even when the higher-layer fuzz still happens to round-trip.
 *
 * References:
 *   RFC 1952 §8 (CRC-32 ISO HDLC, polynomial 0xEDB88320 reflected)
 *   RFC 3309 / RFC 3720 §B.4 (CRC-32c Castagnoli, polynomial 0x1EDC6F41)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nano_crc32.h"
#include "nano_crc32c.h"
#include "nano_test.h"
#include <string.h>

/* setUp / tearDown come from nano_test.h. */

/* ----------------------------------------------------------------
 * CRC-32 (ISO HDLC) — RFC 1952 §8
 * ---------------------------------------------------------------- */

static void test_crc32_empty(void)
{
    /* CRC-32 of zero-length input is 0x00000000 */
    TEST_ASSERT_EQUAL_UINT32(0x00000000u, nano_crc32(NULL, 0));
}

static void test_crc32_check_value(void)
{
    /* Canonical CRC-32 check value (also documented in nano_crc32.h):
     * "123456789" → 0xCBF43926. */
    const uint8_t v[] = "123456789";
    TEST_ASSERT_EQUAL_UINT32(0xCBF43926u, nano_crc32(v, sizeof(v) - 1));
}

static void test_crc32_single_byte_a(void)
{
    /* "a" → 0xE8B7BE43 (well-known vector) */
    const uint8_t v = 'a';
    TEST_ASSERT_EQUAL_UINT32(0xE8B7BE43u, nano_crc32(&v, 1));
}

static void test_crc32_abc(void)
{
    /* "abc" → 0x352441C2 (well-known vector) */
    const uint8_t v[] = "abc";
    TEST_ASSERT_EQUAL_UINT32(0x352441C2u, nano_crc32(v, sizeof(v) - 1));
}

/* ----------------------------------------------------------------
 * CRC-32c (Castagnoli) — RFC 3309 / RFC 3720 §B.4
 * ---------------------------------------------------------------- */

static void test_crc32c_empty(void)
{
    TEST_ASSERT_EQUAL_UINT32(0x00000000u, nano_crc32c(NULL, 0));
}

static void test_crc32c_check_value(void)
{
    /* "123456789" → 0xE3069283 — canonical Castagnoli check value. */
    const uint8_t v[] = "123456789";
    TEST_ASSERT_EQUAL_UINT32(0xE3069283u, nano_crc32c(v, sizeof(v) - 1));
}

static void test_crc32c_iso_zeros(void)
{
    /* RFC 3720 §B.4: 32 bytes of 0x00 → 0x8A9136AA */
    uint8_t buf[32];
    memset(buf, 0x00, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(0x8A9136AAu, nano_crc32c(buf, sizeof(buf)));
}

static void test_crc32c_iso_ones(void)
{
    /* RFC 3720 §B.4: 32 bytes of 0xFF → 0x62A8AB43 */
    uint8_t buf[32];
    memset(buf, 0xFF, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(0x62A8AB43u, nano_crc32c(buf, sizeof(buf)));
}

static void test_crc32c_iso_increasing(void)
{
    /* RFC 3720 §B.4: bytes 0x00..0x1F (incrementing) → 0x46DD794E */
    uint8_t buf[32];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)i;
    }
    TEST_ASSERT_EQUAL_UINT32(0x46DD794Eu, nano_crc32c(buf, sizeof(buf)));
}

static void test_crc32c_iso_decreasing(void)
{
    /* RFC 3720 §B.4: bytes 0x1F..0x00 (decrementing) → 0x113FDB5C */
    uint8_t buf[32];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(31 - i);
    }
    TEST_ASSERT_EQUAL_UINT32(0x113FDB5Cu, nano_crc32c(buf, sizeof(buf)));
}

/* ----------------------------------------------------------------
 * CRC-32c incremental API (used by SCTP zero-copy checksum)
 * ---------------------------------------------------------------- */

static void test_crc32c_incremental_eq_oneshot(void)
{
    /* Splitting the input across multiple update() calls must produce
     * the same digest as the one-shot wrapper. SCTP's zero-copy CRC
     * verifier relies on this for the "header + zeroed-checksum + body"
     * three-segment computation (see nsctp_verify_checksum). */
    uint8_t buf[256];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i ^ 0xA5);
    }
    uint32_t one_shot = nano_crc32c(buf, sizeof(buf));

    uint32_t c = nano_crc32c_init();
    c = nano_crc32c_update(c, buf, 100);
    c = nano_crc32c_update(c, buf + 100, 56);
    c = nano_crc32c_update(c, buf + 156, 100);
    uint32_t segmented = nano_crc32c_final(c);

    TEST_ASSERT_EQUAL_UINT32(one_shot, segmented);
}

static void test_crc32c_incremental_zero_length_segments(void)
{
    /* Zero-length update() calls must not perturb the running digest.
     * SCTP's segmented verifier injects an "all zeros at checksum offset"
     * pseudo-segment whose length might be zero in degenerate inputs. */
    const uint8_t v[] = "123456789";

    uint32_t c = nano_crc32c_init();
    c = nano_crc32c_update(c, NULL, 0);
    c = nano_crc32c_update(c, v, sizeof(v) - 1);
    c = nano_crc32c_update(c, NULL, 0);
    TEST_ASSERT_EQUAL_UINT32(0xE3069283u, nano_crc32c_final(c));
}

static void test_crc32c_incremental_byte_at_a_time(void)
{
    /* Worst-case fragmentation: one byte per update() call. Verifies the
     * inner loop has no per-call setup that depends on length > 0. */
    const uint8_t v[] = "123456789";
    uint32_t c = nano_crc32c_init();
    for (size_t i = 0; i < sizeof(v) - 1; i++) {
        c = nano_crc32c_update(c, &v[i], 1);
    }
    TEST_ASSERT_EQUAL_UINT32(0xE3069283u, nano_crc32c_final(c));
}

/* ----------------------------------------------------------------
 * Test runner
 * ---------------------------------------------------------------- */

int main(void)
{
    UNITY_BEGIN();

    /* CRC-32 ISO HDLC */
    RUN_TEST(test_crc32_empty);
    RUN_TEST(test_crc32_check_value);
    RUN_TEST(test_crc32_single_byte_a);
    RUN_TEST(test_crc32_abc);

    /* CRC-32c Castagnoli */
    RUN_TEST(test_crc32c_empty);
    RUN_TEST(test_crc32c_check_value);
    RUN_TEST(test_crc32c_iso_zeros);
    RUN_TEST(test_crc32c_iso_ones);
    RUN_TEST(test_crc32c_iso_increasing);
    RUN_TEST(test_crc32c_iso_decreasing);

    /* CRC-32c incremental */
    RUN_TEST(test_crc32c_incremental_eq_oneshot);
    RUN_TEST(test_crc32c_incremental_zero_length_segments);
    RUN_TEST(test_crc32c_incremental_byte_at_a_time);

    return UNITY_END();
}
