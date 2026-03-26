/*
 * nanortc — Basic lifecycle tests
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_rtc_internal.h"
#include "nano_crypto.h"
#include "nano_test.h"
#include "nano_test_config.h"
#include <string.h>

/* ---- Tests ---- */

TEST(test_init_destroy)
{
    nano_rtc_t rtc;
    nano_rtc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
    cfg.role = NANO_ROLE_CONTROLLED;
#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
    cfg.jitter_depth_ms = 100;
#endif

    ASSERT_OK(nano_rtc_init(&rtc, &cfg));
    nano_rtc_destroy(&rtc);
}

TEST(test_init_null_params)
{
    nano_rtc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    ASSERT_EQ(nano_rtc_init(NULL, &cfg), NANO_ERR_INVALID_PARAM);
    ASSERT_EQ(nano_rtc_init((nano_rtc_t *)1, NULL), NANO_ERR_INVALID_PARAM);
}

TEST(test_poll_empty)
{
    nano_rtc_t rtc;
    nano_rtc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.crypto = nano_test_crypto();
#if NANORTC_PROFILE >= NANO_PROFILE_AUDIO
    cfg.jitter_depth_ms = 100;
#endif

    ASSERT_OK(nano_rtc_init(&rtc, &cfg));

    nano_output_t out;
    ASSERT_EQ(nano_poll_output(&rtc, &out), NANO_ERR_NO_DATA);

    nano_rtc_destroy(&rtc);
}

TEST(test_byte_order)
{
    ASSERT_EQ(nano_htons(0x0102), 0x0201);
    ASSERT_EQ(nano_ntohs(0x0201), 0x0102);
    ASSERT_EQ(nano_htonl(0x01020304), 0x04030201);
    ASSERT_EQ(nano_ntohl(0x04030201), 0x01020304);
}

TEST(test_crc32c)
{
    /* Known CRC-32c test vector: "123456789" -> 0xE3069283 */
    extern uint32_t nano_crc32c(const uint8_t *data, size_t len);
    const uint8_t data[] = "123456789";
    uint32_t crc = nano_crc32c(data, 9);
    ASSERT_EQ(crc, 0xE3069283);
}

/* ---- Runner ---- */

TEST_MAIN_BEGIN("nanortc basic tests")
    RUN(test_init_destroy);
    RUN(test_init_null_params);
    RUN(test_poll_empty);
    RUN(test_byte_order);
    RUN(test_crc32c);
TEST_MAIN_END
