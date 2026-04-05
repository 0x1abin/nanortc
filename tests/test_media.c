/*
 * nanortc — Media track and SSRC map unit tests
 *
 * Tests track_init(), track_find_by_mid(), ssrc_map_register/lookup()
 * in isolation without requiring a full RTC connection.
 *
 * SPDX-License-Identifier: MIT
 */

#include "nanortc.h"
#include "nano_media.h"
#include "nano_test.h"
#include <string.h>

/* ================================================================
 * track_init
 * ================================================================ */

TEST(test_track_init_audio)
{
    nanortc_track_t m;
    ASSERT_OK(track_init(&m, 0, NANORTC_TRACK_AUDIO, NANORTC_DIR_SENDRECV, 111, 48000, 2, 100));
    ASSERT_EQ(m.mid, 0);
    ASSERT_EQ(m.kind, NANORTC_TRACK_AUDIO);
    ASSERT_EQ(m.direction, NANORTC_DIR_SENDRECV);
    ASSERT_TRUE(m.active);
    ASSERT_EQ(m.codec, 111);
    ASSERT_EQ(m.sample_rate, 48000);
    ASSERT_EQ(m.channels, 2);
}

TEST(test_track_init_video)
{
    nanortc_track_t m;
    ASSERT_OK(track_init(&m, 1, NANORTC_TRACK_VIDEO, NANORTC_DIR_SENDONLY, 96, 90000, 0, 0));
    ASSERT_EQ(m.kind, NANORTC_TRACK_VIDEO);
    ASSERT_EQ(m.mid, 1);
}

TEST(test_track_init_null)
{
    ASSERT_FAIL(track_init(NULL, 0, NANORTC_TRACK_AUDIO, NANORTC_DIR_SENDRECV, 111, 48000, 2, 100));
}

/* ================================================================
 * track_find_by_mid
 * ================================================================ */

TEST(test_track_find_by_mid_found)
{
    nanortc_track_t media[3];
    memset(media, 0, sizeof(media));
    track_init(&media[0], 0, NANORTC_TRACK_AUDIO, NANORTC_DIR_SENDRECV, 111, 48000, 2, 100);
    track_init(&media[1], 1, NANORTC_TRACK_VIDEO, NANORTC_DIR_SENDONLY, 96, 90000, 0, 0);
    track_init(&media[2], 2, NANORTC_TRACK_AUDIO, NANORTC_DIR_RECVONLY, 0, 8000, 1, 100);

    nanortc_track_t *found = track_find_by_mid(media, 3, 1);
    ASSERT_TRUE(found != NULL);
    ASSERT_EQ(found->mid, 1);
    ASSERT_EQ(found->kind, NANORTC_TRACK_VIDEO);
}

TEST(test_track_find_by_mid_not_found)
{
    nanortc_track_t media[2];
    memset(media, 0, sizeof(media));
    track_init(&media[0], 0, NANORTC_TRACK_AUDIO, NANORTC_DIR_SENDRECV, 111, 48000, 2, 100);
    track_init(&media[1], 1, NANORTC_TRACK_VIDEO, NANORTC_DIR_SENDONLY, 96, 90000, 0, 0);

    ASSERT_EQ(track_find_by_mid(media, 2, 99), NULL);
}

TEST(test_track_find_by_mid_inactive)
{
    nanortc_track_t media[1];
    memset(media, 0, sizeof(media));
    track_init(&media[0], 0, NANORTC_TRACK_AUDIO, NANORTC_DIR_SENDRECV, 111, 48000, 2, 100);
    media[0].active = false;

    ASSERT_EQ(track_find_by_mid(media, 1, 0), NULL);
}

TEST(test_track_find_by_mid_null)
{
    ASSERT_EQ(track_find_by_mid(NULL, 0, 0), NULL);
}

/* ================================================================
 * ssrc_map_register / ssrc_map_lookup
 * ================================================================ */

TEST(test_ssrc_map_register_lookup)
{
    nanortc_ssrc_entry_t map[4];
    memset(map, 0, sizeof(map));

    ASSERT_OK(ssrc_map_register(map, 4, 0x12345678, 0));
    ASSERT_OK(ssrc_map_register(map, 4, 0xAABBCCDD, 1));

    ASSERT_EQ(ssrc_map_lookup(map, 4, 0x12345678), 0);
    ASSERT_EQ(ssrc_map_lookup(map, 4, 0xAABBCCDD), 1);
    ASSERT_EQ(ssrc_map_lookup(map, 4, 0xDEADBEEF), -1); /* Not found */
}

TEST(test_ssrc_map_update)
{
    nanortc_ssrc_entry_t map[4];
    memset(map, 0, sizeof(map));

    ASSERT_OK(ssrc_map_register(map, 4, 0x12345678, 0));
    ASSERT_EQ(ssrc_map_lookup(map, 4, 0x12345678), 0);

    /* Update existing entry */
    ASSERT_OK(ssrc_map_register(map, 4, 0x12345678, 2));
    ASSERT_EQ(ssrc_map_lookup(map, 4, 0x12345678), 2);
}

TEST(test_ssrc_map_full)
{
    nanortc_ssrc_entry_t map[2];
    memset(map, 0, sizeof(map));

    ASSERT_OK(ssrc_map_register(map, 2, 1, 0));
    ASSERT_OK(ssrc_map_register(map, 2, 2, 1));
    ASSERT_FAIL(ssrc_map_register(map, 2, 3, 2)); /* Full */
}

TEST(test_ssrc_map_null)
{
    ASSERT_FAIL(ssrc_map_register(NULL, 4, 1, 0));
    ASSERT_EQ(ssrc_map_lookup(NULL, 4, 1), -1);
}

/* ================================================================
 * Test runner
 * ================================================================ */

TEST_MAIN_BEGIN("test_media")
/* Track init */
RUN(test_track_init_audio);
RUN(test_track_init_video);
RUN(test_track_init_null);
/* Track find */
RUN(test_track_find_by_mid_found);
RUN(test_track_find_by_mid_not_found);
RUN(test_track_find_by_mid_inactive);
RUN(test_track_find_by_mid_null);
/* SSRC map */
RUN(test_ssrc_map_register_lookup);
RUN(test_ssrc_map_update);
RUN(test_ssrc_map_full);
RUN(test_ssrc_map_null);
TEST_MAIN_END
