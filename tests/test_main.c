/*
 * nanortc — Basic lifecycle tests
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nanortc_crypto.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

/* ---- Tests ---- */

TEST(test_init_destroy)
{
    nanortc_t rtc;
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANORTC_ROLE_CONTROLLED;
#if NANORTC_FEATURE_AUDIO
    cfg.jitter_depth_ms = 100;
#endif

    ASSERT_OK(nanortc_init(&rtc, &cfg));
    nanortc_destroy(&rtc);
}

TEST(test_init_null_params)
{
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    ASSERT_EQ(nanortc_init(NULL, &cfg), NANORTC_ERR_INVALID_PARAM);
    ASSERT_EQ(nanortc_init((nanortc_t *)1, NULL), NANORTC_ERR_INVALID_PARAM);
}

TEST(test_poll_empty)
{
    nanortc_t rtc;
    nanortc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
#if NANORTC_FEATURE_AUDIO
    cfg.jitter_depth_ms = 100;
#endif

    ASSERT_OK(nanortc_init(&rtc, &cfg));

    nanortc_output_t out;
    ASSERT_EQ(nanortc_poll_output(&rtc, &out), NANORTC_ERR_NO_DATA);

    nanortc_destroy(&rtc);
}

TEST(test_byte_order)
{
    ASSERT_EQ(nanortc_htons(0x0102), 0x0201);
    ASSERT_EQ(nanortc_ntohs(0x0201), 0x0102);
    ASSERT_EQ(nanortc_htonl(0x01020304), 0x04030201);
    ASSERT_EQ(nanortc_ntohl(0x04030201), 0x01020304);
}

#if NANORTC_FEATURE_DATACHANNEL
TEST(test_crc32c)
{
    /* Known CRC-32c test vector: "123456789" -> 0xE3069283 */
    extern uint32_t nano_crc32c(const uint8_t *data, size_t len);
    const uint8_t data[] = "123456789";
    uint32_t crc = nano_crc32c(data, 9);
    ASSERT_EQ(crc, 0xE3069283);
}
#endif

TEST(test_crc32)
{
    /* Known CRC-32 (ISO HDLC) test vector: "123456789" -> 0xCBF43926 */
    extern uint32_t nano_crc32(const uint8_t *data, size_t len);
    const uint8_t data[] = "123456789";
    uint32_t crc = nano_crc32(data, 9);
    ASSERT_EQ(crc, 0xCBF43926);
}

/* ---- Runner ---- */

TEST_MAIN_BEGIN("nanortc basic tests")
    RUN(test_init_destroy);
    RUN(test_init_null_params);
    RUN(test_poll_empty);
    RUN(test_byte_order);
#if NANORTC_FEATURE_DATACHANNEL
    RUN(test_crc32c);
#endif
    RUN(test_crc32);
TEST_MAIN_END
